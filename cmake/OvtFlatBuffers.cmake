# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# FlatBuffers code generation (chapter 13 WI-0.003, chapter 16 §16.3.10). One file, one function; WI-0.083
# extends this file (.bfbs embedding, schema_version.h) and does not add a second one.
#
#   ovt_add_flatbuffers_schema(<target> <fbs>...)
#
# Creates the INTERFACE library <target> (conventionally ovt_schemas) and, for every schema, one custom
# command that runs flatc three times, all below the generated-code root of chapter 13 §13.2.3:
#   C++     ${OVT_GENERATED_DIR}/<schema>_generated.h  (include path: ${OVT_GENERATED_DIR})
#   Python  ${OVT_GENERATED_DIR}/python/ovt/fb/<Type>.py + __init__.py  (namespace layout, no --gen-onefile)
#   Kotlin  ${OVT_GENERATED_DIR}/kotlin/
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

function(ovt_add_flatbuffers_schema target)
  set(schemas ${ARGN})
  if(NOT schemas)
    message(FATAL_ERROR "ovt_add_flatbuffers_schema(${target}): at least one .fbs file is required.")
  endif()
  if(NOT OVT_FLATC_EXECUTABLE)
    _ovt_find_flatc()
    set(OVT_FLATC_EXECUTABLE "${OVT_FLATC_EXECUTABLE}" CACHE INTERNAL "resolved flatc")
  endif()

  set(gen_dir "${OVT_GENERATED_DIR}")
  set(outputs "")
  foreach(fbs IN LISTS schemas)
    cmake_path(ABSOLUTE_PATH fbs NORMALIZE)
    get_filename_component(stem "${fbs}" NAME_WE)
    get_filename_component(fbs_dir "${fbs}" DIRECTORY)
    set(header "${gen_dir}/${stem}_generated.h")
    # Stamp for the Python/Kotlin files, whose names depend on the schema's types.
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_${stem}.stamp")
    add_custom_command(
      OUTPUT "${header}" "${stamp}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}/python" "${gen_dir}/kotlin"
      COMMAND "${OVT_FLATC_EXECUTABLE}" --cpp --gen-object-api --gen-compare --reflect-names --scoped-enums -I
              "${fbs_dir}" -o "${gen_dir}" "${fbs}"
      COMMAND "${OVT_FLATC_EXECUTABLE}" --python -I "${fbs_dir}" -o "${gen_dir}/python" "${fbs}"
      COMMAND "${OVT_FLATC_EXECUTABLE}" --kotlin -I "${fbs_dir}" -o "${gen_dir}/kotlin" "${fbs}"
      COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
      DEPENDS ${schemas}
      COMMENT "flatc: ${stem}.fbs -> C++, Python, Kotlin"
      VERBATIM)
    list(APPEND outputs "${header}" "${stamp}")
  endforeach()

  # An INTERFACE library with PRIVATE sources is a real build target (CMake 3.19), so both
  # `cmake --build --target <target>` and target_link_libraries(<consumer> <target>) work. The headers are
  # generated sources: nothing compiles them, but linking the target orders the consumer after generation.
  add_library(${target} INTERFACE)
  target_sources(${target} PRIVATE ${outputs})
  # SYSTEM: flatc output is not ours to warn about (it trips -Wuseless-cast under ovt::flags with -Werror).
  target_include_directories(${target} SYSTEM INTERFACE "$<BUILD_INTERFACE:${gen_dir}>")
endfunction()
