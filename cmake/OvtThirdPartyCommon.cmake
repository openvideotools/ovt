# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Shared helpers of the third_party/*.cmake ExternalProject scripts (chapter 13 §13.3.4, WI-0.010).
# third_party/_template.cmake shows them in use.
#
#   Configure time: tools/versions_to_cmake.py turns OVT_TP_VERSIONS_MD (default third_party/VERSIONS.md)
#   into ${CMAKE_BINARY_DIR}/third_party/versions.cmake (= build/<preset>/third_party/versions.cmake), which
#   is included here. It defines OVT_TP_<dep>_{VERSION,URL,SHA256,LICENSE,PATCHES,ROW} per VERSIONS.md row and
#   OVT_TP_DEPS. Editing VERSIONS.md re-runs configure. A version or hash is never written anywhere else.
#
#   ovt_tp_declare(<dep> [URL <url>] [SHA256 <hex>] [LICENSE <spdx>] [LICENSE_FILES <file>...] [NO_DOWNLOAD])
#       Declares the use of the VERSIONS.md row <dep> (the lower-cased id: libaom, ffmpeg, ...). URL, SHA256
#       and LICENSE default to the row's values and, when given, must equal them. Fails when the row is
#       missing or has no SHA-256 yet. Downloads the archive into OVT_TP_DOWNLOADS at configure time and
#       verifies its SHA-256; a mismatch fails configure naming the row (§13.3.7: every hash is verified).
#       Sets OVT_TP_<dep>_ARCHIVE to the local archive path. NO_DOWNLOAD skips the fetch (prebuilt kits
#       verified by their own installer). Records the licence for ovt_tp_import_static().
#
#   ovt_tp_apply_patches(<out-var> (DIR <dir> | SERIES <file>) [INSTALL_DIR <dir>])
#       Sets <out-var> to a PATCH_COMMAND for ExternalProject_Add that runs tools/apply_patches.py --check:
#       every *.patch of DIR in lexical order (no-op when DIR is empty or missing), or the quilt-style SERIES
#       file. INSTALL_DIR adds --record <INSTALL_DIR>/share/ovt/patches.json (§13.3.6).
#
#   OVT_TP_PATCH
#       The command prefix of the §13.3.4 template: `PATCH_COMMAND ${OVT_TP_PATCH} <patch-dir>`.
#
#   OVT_TP_CMAKE_TOOLCHAIN_ARGS
#       CMake arguments that forward the active toolchain (compilers, toolchain file, OSX/Android settings,
#       build type) into CMake-based ExternalProjects.
#
#   OVT_TP_SANITIZER_C_FLAGS, OVT_TP_SANITIZER_LINK_FLAGS
#       The -fsanitize/-fno-sanitize-recover/-fno-omit-frame-pointer flags of the active OVT_SANITIZER set (§13.2.5),
#       space-separated, empty when OVT_ENABLE_SANITIZERS=OFF. OVT_TP_CMAKE_TOOLCHAIN_ARGS already carries them as
#       CMAKE_{C,CXX}_FLAGS and CMAKE_{EXE,SHARED,MODULE}_LINKER_FLAGS; a script that does not use
#       CMake (Meson, configure) passes these two variables on itself (§14.8.1, WI-0.558). A script must not add its
#       own CMAKE_{C,CXX}_FLAGS to CMAKE_ARGS: the later -D replaces the other, silently dropping the sanitizer flags.
#
#   ovt_tp_import_static(<imported-target> <ep-target> <library-relpath> <include-relpath>)
#       Creates the STATIC IMPORTED GLOBAL target <imported-target> (tp::aom, ...) for a library installed by
#       ExternalProject <ep-target> under its INSTALL_DIR, makes it depend on the build, and describes it for
#       the licence manifest with the licence and version of the row declared for <ep-target> (tp_<dep>).

include_guard(GLOBAL)
include(ExternalProject)

find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)

set(_OVT_TP_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
cmake_path(NORMAL_PATH _OVT_TP_ROOT)
set(_OVT_TP_TOOLS "${_OVT_TP_ROOT}tools")
set(OVT_TP_VERSIONS_MD "${_OVT_TP_ROOT}third_party/VERSIONS.md"
    CACHE FILEPATH "Third-party pin table parsed into versions.cmake (chapter 13 §13.3.7)")
set(OVT_TP_VERSIONS_CMAKE "${CMAKE_BINARY_DIR}/third_party/versions.cmake")

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -I "${_OVT_TP_TOOLS}/versions_to_cmake.py" --versions "${OVT_TP_VERSIONS_MD}" --out
          "${OVT_TP_VERSIONS_CMAKE}" RESULT_VARIABLE _ovt_tp_rc ERROR_VARIABLE _ovt_tp_err)
if(NOT _ovt_tp_rc EQUAL 0)
  message(FATAL_ERROR "tools/versions_to_cmake.py failed on ${OVT_TP_VERSIONS_MD}:\n${_ovt_tp_err}")
endif()
unset(_ovt_tp_rc)
unset(_ovt_tp_err)
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${OVT_TP_VERSIONS_MD}"
                                                             "${_OVT_TP_TOOLS}/versions_to_cmake.py")
include("${OVT_TP_VERSIONS_CMAKE}")

set(OVT_TP_PATCH "${Python3_EXECUTABLE}" -I "${_OVT_TP_TOOLS}/apply_patches.py" --check --dir)

# ---- toolchain forwarding ---------------------------------------------------------------------------
set(OVT_TP_CMAKE_TOOLCHAIN_ARGS
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" "-DCMAKE_C_VISIBILITY_PRESET=hidden" "-DCMAKE_CXX_VISIBILITY_PRESET=hidden")
if(CMAKE_BUILD_TYPE)
  # Third-party code is always built optimised; Debug presets debug OVT, not libaom.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND OVT_TP_CMAKE_TOOLCHAIN_ARGS "-DCMAKE_BUILD_TYPE=RelWithDebInfo")
  else()
    list(APPEND OVT_TP_CMAKE_TOOLCHAIN_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
  endif()
endif()
foreach(
  _var
  CMAKE_C_COMPILER_LAUNCHER
  CMAKE_CXX_COMPILER_LAUNCHER
  CMAKE_OSX_ARCHITECTURES
  CMAKE_OSX_DEPLOYMENT_TARGET
  CMAKE_OSX_SYSROOT
  CMAKE_SYSTEM_NAME
  CMAKE_SYSTEM_PROCESSOR
  ANDROID_ABI
  ANDROID_PLATFORM
  ANDROID_STL)
  if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
    list(APPEND OVT_TP_CMAKE_TOOLCHAIN_ARGS "-D${_var}=${${_var}}")
  endif()
endforeach()
# The vcpkg toolchain chain-loads the real one (Android NDK); pass the chain-loaded file, not vcpkg's.
if(VCPKG_CHAINLOAD_TOOLCHAIN_FILE)
  list(APPEND OVT_TP_CMAKE_TOOLCHAIN_ARGS "-DCMAKE_TOOLCHAIN_FILE=${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}")
elseif(CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE MATCHES "/vcpkg\\.cmake$")
  list(APPEND OVT_TP_CMAKE_TOOLCHAIN_ARGS "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif()
unset(_var)

# ---- sanitizer flags --------------------------------------------------------------------------------
# Third-party code is built with the same -fsanitize flags as OVT (§14.8.1); otherwise ASan reports false
# positives at the ABI boundary (shadow memory of instrumented and uninstrumented code disagree) and TSan
# misses races inside libaom and FFmpeg. Optimisation and debug flags stay with the build type above.
include(OvtTesting)
set(OVT_TP_SANITIZER_C_FLAGS "")
set(OVT_TP_SANITIZER_LINK_FLAGS "")
foreach(_flag IN LISTS OVT_SANITIZER_COMPILE_FLAGS)
  if(_flag MATCHES "^-f(no-)?sanitize|^-fno-omit-frame-pointer$")
    string(APPEND OVT_TP_SANITIZER_C_FLAGS " ${_flag}")
  endif()
endforeach()
foreach(_flag IN LISTS OVT_SANITIZER_LINK_FLAGS)
  string(APPEND OVT_TP_SANITIZER_LINK_FLAGS " ${_flag}")
endforeach()
string(STRIP "${OVT_TP_SANITIZER_C_FLAGS}" OVT_TP_SANITIZER_C_FLAGS)
string(STRIP "${OVT_TP_SANITIZER_LINK_FLAGS}" OVT_TP_SANITIZER_LINK_FLAGS)
unset(_flag)
if(OVT_TP_SANITIZER_C_FLAGS)
  list(
    APPEND
    OVT_TP_CMAKE_TOOLCHAIN_ARGS
    "-DCMAKE_C_FLAGS=${OVT_TP_SANITIZER_C_FLAGS}"
    "-DCMAKE_CXX_FLAGS=${OVT_TP_SANITIZER_C_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${OVT_TP_SANITIZER_LINK_FLAGS}"
    "-DCMAKE_SHARED_LINKER_FLAGS=${OVT_TP_SANITIZER_LINK_FLAGS}"
    "-DCMAKE_MODULE_LINKER_FLAGS=${OVT_TP_SANITIZER_LINK_FLAGS}")
endif()

# ---- ovt_tp_declare ---------------------------------------------------------------------------------
function(ovt_tp_declare dep)
  cmake_parse_arguments(PARSE_ARGV 1 arg "NO_DOWNLOAD" "URL;SHA256;LICENSE" "LICENSE_FILES")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "ovt_tp_declare(${dep}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
  endif()
  if(arg_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "ovt_tp_declare(${dep}): ${arg_KEYWORDS_MISSING_VALUES} has no value. A licence file named "
                        "LICENSE must be written ./LICENSE, or it is read as the LICENSE keyword.")
  endif()
  if(NOT DEFINED OVT_TP_${dep}_ROW)
    message(FATAL_ERROR "ovt_tp_declare(${dep}): third_party/VERSIONS.md has no row with id '${dep}' "
                        "(known: ${OVT_TP_DEPS}).")
  endif()
  set(row "${OVT_TP_${dep}_ROW}")
  foreach(field URL SHA256 LICENSE)
    if(DEFINED arg_${field} AND NOT "${arg_${field}}" STREQUAL "${OVT_TP_${dep}_${field}}")
      message(FATAL_ERROR "ovt_tp_declare(${dep}): ${field} '${arg_${field}}' differs from ${row} "
                          "('${OVT_TP_${dep}_${field}}'). Change VERSIONS.md, not the script.")
    endif()
  endforeach()
  set_property(GLOBAL PROPERTY OVT_TP_LICENSE_tp_${dep} "${OVT_TP_${dep}_LICENSE}")
  set_property(GLOBAL PROPERTY OVT_TP_VERSION_tp_${dep} "${OVT_TP_${dep}_VERSION}")
  set_property(GLOBAL PROPERTY OVT_TP_LICENSE_FILES_tp_${dep} "${arg_LICENSE_FILES}")
  if(OVT_TP_${dep}_PATCHES)
    set_property(GLOBAL PROPERTY OVT_TP_PATCHED_tp_${dep} TRUE) # licence manifest: "modified"
  endif()
  if(arg_NO_DOWNLOAD)
    return()
  endif()

  set(url "${OVT_TP_${dep}_URL}")
  set(sha "${OVT_TP_${dep}_SHA256}")
  if(url STREQUAL "")
    message(FATAL_ERROR "ovt_tp_declare(${dep}): ${row} has no URL.")
  endif()
  if(sha STREQUAL "")
    message(FATAL_ERROR "ovt_tp_declare(${dep}): ${row} has no SHA-256. Run "
                        "`python3 -I tools/versions_to_cmake.py --update-hashes` and commit VERSIONS.md.")
  endif()
  string(REGEX REPLACE "^.*/" "" file_name "${url}")
  set(archive "${OVT_TP_DOWNLOADS}/${file_name}")
  if(EXISTS "${archive}")
    file(SHA256 "${archive}" have)
    if(NOT have STREQUAL sha)
      message(FATAL_ERROR "ovt_tp_declare(${dep}): ${archive} has SHA-256 ${have}, but ${row} pins ${sha}. "
                          "Delete the file to download it again, or fix the row with --update-hashes.")
    endif()
  else()
    message(STATUS "Downloading ${dep} ${OVT_TP_${dep}_VERSION}: ${url}")
    # Transient network failures (timeouts, TLS handshakes, 5xx) are retried with exponential backoff; the
    # SHA-256 check below is unchanged and a mismatch is never retried.
    set(max_attempts 5)
    set(delay 5)
    foreach(attempt RANGE 1 ${max_attempts})
      file(
        DOWNLOAD "${url}" "${archive}.part"
        STATUS status
        TLS_VERIFY ON
        TIMEOUT 900
        INACTIVITY_TIMEOUT 60)
      list(GET status 0 code)
      if(code EQUAL 0)
        break()
      endif()
      list(GET status 1 msg)
      file(REMOVE "${archive}.part")
      if(attempt LESS max_attempts)
        message(
          STATUS "Download of ${url} failed (${msg}); attempt ${attempt} of ${max_attempts}, retrying in ${delay} s")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${delay})
        math(EXPR delay "${delay} * 2")
      endif()
    endforeach()
    if(NOT code EQUAL 0)
      message(
        FATAL_ERROR "ovt_tp_declare(${dep}): download of ${url} (${row}) failed after ${max_attempts} attempts: "
                    "${msg}. Offline builds pre-fetch with tools/third_party_offline.py --out ${OVT_TP_DOWNLOADS}.")
    endif()
    file(SHA256 "${archive}.part" have)
    if(NOT have STREQUAL sha)
      file(REMOVE "${archive}.part")
      message(FATAL_ERROR "ovt_tp_declare(${dep}): SHA-256 mismatch for ${url}: got ${have}, ${row} pins " "${sha}.")
    endif()
    file(RENAME "${archive}.part" "${archive}")
  endif()
  set(OVT_TP_${dep}_ARCHIVE "${archive}" PARENT_SCOPE)
endfunction()

# ---- ovt_tp_apply_patches ---------------------------------------------------------------------------
function(ovt_tp_apply_patches out_var)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "DIR;SERIES;INSTALL_DIR" "")
  if((arg_DIR AND arg_SERIES) OR (NOT arg_DIR AND NOT arg_SERIES))
    message(FATAL_ERROR "ovt_tp_apply_patches(${out_var}): give exactly one of DIR or SERIES.")
  endif()
  set(cmd "${Python3_EXECUTABLE}" -I "${_OVT_TP_TOOLS}/apply_patches.py" --check)
  if(arg_DIR)
    list(APPEND cmd --dir "${arg_DIR}")
  else()
    list(APPEND cmd --series "${arg_SERIES}")
  endif()
  if(arg_INSTALL_DIR)
    list(APPEND cmd --record "${arg_INSTALL_DIR}/share/ovt/patches.json")
  endif()
  set(${out_var} ${cmd} PARENT_SCOPE)
endfunction()

# ---- ovt_tp_import_static ---------------------------------------------------------------------------
function(ovt_tp_import_static imported ep library include_dir)
  if(NOT TARGET ${ep})
    message(FATAL_ERROR "ovt_tp_import_static(${imported}): ExternalProject '${ep}' does not exist.")
  endif()
  get_property(spdx GLOBAL PROPERTY OVT_TP_LICENSE_${ep})
  get_property(version GLOBAL PROPERTY OVT_TP_VERSION_${ep})
  if(NOT spdx)
    message(FATAL_ERROR "ovt_tp_import_static(${imported}): no ovt_tp_declare() for '${ep}' "
                        "(ExternalProject names are tp_<dep>).")
  endif()
  externalproject_get_property(${ep} INSTALL_DIR)
  # INTERFACE_INCLUDE_DIRECTORIES must exist at generate time, before the build installs anything.
  file(MAKE_DIRECTORY "${INSTALL_DIR}/${include_dir}")
  add_library(${imported} STATIC IMPORTED GLOBAL)
  set_target_properties(${imported} PROPERTIES IMPORTED_LOCATION "${INSTALL_DIR}/${library}"
                                               INTERFACE_INCLUDE_DIRECTORIES "${INSTALL_DIR}/${include_dir}")
  add_dependencies(${imported} ${ep})
  set(patched "")
  get_property(was_patched GLOBAL PROPERTY OVT_TP_PATCHED_${ep})
  if(was_patched)
    set(patched PATCHED)
  endif()
  ovt_set_dependency_license(
    ${imported}
    SOURCE
    third_party
    SPDX
    "${spdx}"
    VERSION
    "${version}"
    LINKAGE
    static
    ${patched})
endfunction()
