# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# FlatBuffers code generation (chapter 13 WI-0.003, chapter 16 §16.3.10). One file, one function; WI-0.083
# extended it with the binary schemas and their embedding, and does not add a second file.
#
#   ovt_add_flatbuffers_schema(<target> <fbs>...)
#
# Creates the INTERFACE library <target> (conventionally ovt_schemas) and, for every schema, one custom
# command that runs flatc four times, all below the generated-code root of chapter 13 §13.2.3:
#   C++     ${OVT_GENERATED_DIR}/<schema>_generated.h  (include path: ${OVT_GENERATED_DIR})
#   Python  ${OVT_GENERATED_DIR}/python/ovt/fb/<Type>.py + __init__.py  (namespace layout, no --gen-onefile)
#   Kotlin  ${OVT_GENERATED_DIR}/kotlin/
#   bfbs    ${OVT_GENERATED_DIR}/<schema>.bfbs  (binary schema with doc comments, WI-0.083)
# plus one command that embeds every .bfbs of the call as byte arrays (tools/embed_bfbs.py) in
#   ${OVT_GENERATED_DIR}/<target>_bfbs.inc  (k_<schema>_bfbs[] and OVT_BFBS_HAVE_<SCHEMA>; read by
#   core/src/model/bfbs_resources.cpp for ovt_schemas)
# The target's properties OVT_BFBS_FILES and OVT_BFBS_INC name those outputs.
# The schemas of one call may include each other: every command depends on all of them.
# Linking <target> adds the generated include directory, the flatbuffers headers and the dependency on
# the generation, so a consumer only writes #include "ovt_model_generated.h".
#
# flatc is always the HOST tool (Android cross builds must not pick the target-triplet binary): the vcpkg
# manifest lists flatbuffers with "host": true, and find_program() ignores the toolchain's root path.
# Override with -DOVT_FLATC=/path/to/flatc.
#
# SPEC-ASSUMPTION(#32): Python and Kotlin are generated in every configuration, not only under
# OVT_BUILD_PYTHON / Android, because acceptance test 1 of WI-0.003 expects FrameAnalysis.py after a plain
# linux-x64-debug build (whose preset sets OVT_BUILD_PYTHON=OFF).

include_guard(GLOBAL)

set(OVT_FLATC "" CACHE FILEPATH "flatc executable that generates the schema code (host tool; empty = search)")

function(_ovt_find_flatc)
  if(OVT_FLATC)
    set(_ovt_flatc "${OVT_FLATC}")
  else()
    set(_ovt_flatc "")
    # vcpkg manifest mode: the "host": true flatbuffers port puts flatc under installed/<host-triplet>/tools/
    # flatbuffers, next to the target triplets' trees. The host triplet's name is not reliable to read back
    # from the toolchain, so take the first candidate that RUNS on this machine: a target-triplet flatc
    # (Android) cannot, which is exactly the one never to pick.
    if(DEFINED _VCPKG_INSTALLED_DIR)
      file(GLOB _candidates "${_VCPKG_INSTALLED_DIR}/*/tools/flatbuffers/flatc")
      foreach(_c IN LISTS _candidates)
        execute_process(COMMAND "${_c}" --version RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
        if(_rc EQUAL 0)
          set(_ovt_flatc "${_c}")
          break()
        endif()
      endforeach()
    endif()
    if(NOT _ovt_flatc)
      unset(_ovt_flatc_found CACHE)
      find_program(_ovt_flatc_found NAMES flatc NO_CMAKE_FIND_ROOT_PATH)
      set(_ovt_flatc "${_ovt_flatc_found}")
    endif()
  endif()
  if(NOT _ovt_flatc)
    message(FATAL_ERROR "flatc not found. It comes with the vcpkg 'flatbuffers' host port, or "
                        "'sudo apt-get install flatbuffers-compiler'; or pass -DOVT_FLATC=<path>.")
  endif()
  set(OVT_FLATC_EXECUTABLE "${_ovt_flatc}" PARENT_SCOPE)
endfunction()

# The generated headers only compile against the FlatBuffers library of the same version (they
# static_assert on it); say so at configure time instead (WI-0.083 notes). Checked once per configure and
# only when both versions are known (a pkg-config or header-only flatbuffers may not report one).
function(_ovt_check_flatc_version)
  get_property(_checked GLOBAL PROPERTY OVT_FLATC_VERSION_CHECKED)
  # Recorded by ovt_find_package(flatbuffers ...) through ovt_set_dependency_license (cmake/OvtLicenseTags.cmake).
  get_property(_lib_version GLOBAL PROPERTY OVT_DEP_VERSION_flatbuffers)
  if(_checked OR NOT _lib_version)
    return()
  endif()
  set_property(GLOBAL PROPERTY OVT_FLATC_VERSION_CHECKED ON)
  execute_process(COMMAND "${OVT_FLATC_EXECUTABLE}" --version OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_QUIET)
  string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" _flatc_version "${_out}")
  if(_rc EQUAL 0 AND _flatc_version AND NOT _flatc_version VERSION_EQUAL _lib_version)
    message(FATAL_ERROR "flatc ${_flatc_version} (${OVT_FLATC_EXECUTABLE}) does not match the FlatBuffers library "
                        "${_lib_version}. Use the flatc of the same release (vcpkg host port or -DOVT_FLATC=...).")
  endif()
endfunction()

function(ovt_add_flatbuffers_schema target)
  set(schemas ${ARGN})
  if(NOT schemas)
    message(FATAL_ERROR "ovt_add_flatbuffers_schema(${target}): at least one .fbs file is required.")
  endif()
  if(NOT OVT_FLATC_EXECUTABLE)
    _ovt_find_flatc()
    set(OVT_FLATC_EXECUTABLE "${OVT_FLATC_EXECUTABLE}" CACHE INTERNAL "resolved flatc")
  endif()

  _ovt_check_flatc_version()

  set(gen_dir "${OVT_GENERATED_DIR}")
  set(outputs "")
  set(bfbs_files "")
  foreach(fbs IN LISTS schemas)
    cmake_path(ABSOLUTE_PATH fbs NORMALIZE)
    get_filename_component(stem "${fbs}" NAME_WE)
    get_filename_component(fbs_dir "${fbs}" DIRECTORY)
    set(header "${gen_dir}/${stem}_generated.h")
    # Stamp for the Python/Kotlin files, whose names depend on the schema's types.
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_${stem}.stamp")
    set(bfbs "${gen_dir}/${stem}.bfbs")
    add_custom_command(
      OUTPUT "${header}" "${stamp}" "${bfbs}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}/python" "${gen_dir}/kotlin"
      # --gen-name-strings adds T::GetFullyQualifiedName(), which ovt::model::ext_as<T> compares against
      # Block.ext_type (chapter 06 §6.3.3, WI-0.084).
      COMMAND "${OVT_FLATC_EXECUTABLE}" --cpp --gen-object-api --gen-compare --reflect-names --gen-name-strings
              --scoped-enums -I "${fbs_dir}" -o "${gen_dir}" "${fbs}"
      COMMAND "${OVT_FLATC_EXECUTABLE}" --python -I "${fbs_dir}" -o "${gen_dir}/python" "${fbs}"
      COMMAND "${OVT_FLATC_EXECUTABLE}" --kotlin -I "${fbs_dir}" -o "${gen_dir}/kotlin" "${fbs}"
      # --bfbs-filenames pins the declaration_file fields to paths relative to the schema directory
      # ("//ovt_model.fbs"), so the bytes do not depend on where the tree is checked out (§0.7.5).
      COMMAND "${OVT_FLATC_EXECUTABLE}" --binary --schema --bfbs-comments --bfbs-filenames "${fbs_dir}" -I "${fbs_dir}"
              -o "${gen_dir}" "${fbs}"
      COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
      DEPENDS ${schemas}
      COMMENT "flatc: ${stem}.fbs -> C++, Python, Kotlin, bfbs"
      VERBATIM)
    list(APPEND outputs "${header}" "${stamp}" "${bfbs}")
    list(APPEND bfbs_files "${bfbs}")
  endforeach()

  # Embedding (WI-0.083): one include fragment per call, rewritten only when its bytes change.
  find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
  set(bfbs_inc "${gen_dir}/${target}_bfbs.inc")
  set(embed_script "${PROJECT_SOURCE_DIR}/tools/embed_bfbs.py")
  add_custom_command(
    OUTPUT "${bfbs_inc}"
    COMMAND "${Python3_EXECUTABLE}" -I "${embed_script}" --out "${bfbs_inc}" ${bfbs_files}
    DEPENDS ${bfbs_files} "${embed_script}"
    COMMENT "embed_bfbs: ${target} -> ${target}_bfbs.inc"
    VERBATIM)
  list(APPEND outputs "${bfbs_inc}")

  # An INTERFACE library with PRIVATE sources is a real build target (CMake 3.19), so both
  # `cmake --build --target <target>` and target_link_libraries(<consumer> <target>) work. The headers are
  # generated sources: nothing compiles them, but linking the target orders the consumer after generation.
  add_library(${target} INTERFACE)
  target_sources(${target} PRIVATE ${outputs})
  # SYSTEM: flatc output is not ours to warn about (it trips -Wuseless-cast under ovt::flags with -Werror).
  target_include_directories(${target} SYSTEM INTERFACE "$<BUILD_INTERFACE:${gen_dir}>")
  # The generated headers include <flatbuffers/flatbuffers.h>: linking the schema target brings the library
  # along (set by ovt_find_package(flatbuffers) in the including directory tree).
  if(flatbuffers_TARGET)
    target_link_libraries(${target} INTERFACE "$<BUILD_INTERFACE:${flatbuffers_TARGET}>")
  endif()
  set_target_properties(${target} PROPERTIES OVT_BFBS_FILES "${bfbs_files}" OVT_BFBS_INC "${bfbs_inc}")
endfunction()
