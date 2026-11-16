# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Script mode (cmake -DSRC=<libaom source dir> -P nasm3_check.cmake), run by third_party/libaom.cmake between the
# patch and configure steps. libaom's test_nasm() (build/cmake/aom_optimization.cmake) refuses a NASM whose
# `nasm -hf` text lacks "-Ox". NASM 3.x no longer lists -Ox there, although multipass optimisation is its default
# (since 2.09), so configure fails with "Unsupported nasm: multipass optimization not supported" on Homebrew NASM
# 3.02 (macOS x86_64 builds; WI-0.063). This rewrites only that one condition to also accept a NASM whose `nasm -v`
# reports version 3 or later. The object-format checks after it are untouched. It is a build-system edit, not a
# decoder change, so it is not one of the plugin's extension patches (OVT_AOM_PATCH_DIR) and is not recorded in
# patches.json. Idempotent; fails if the expected line is missing, so a pin bump that changes it is noticed. Drop
# this step once the libaom pin carries an upstream fix.

if(NOT SRC)
  message(FATAL_ERROR "nasm3_check.cmake: pass -DSRC=<libaom source dir>")
endif()
set(_file "${SRC}/build/cmake/aom_optimization.cmake")
file(READ "${_file}" _text)
set(_old [[  if(NOT "${nasm_helptext}" MATCHES "-Ox")]])
set(_marker "OVT nasm3_check")
string(FIND "${_text}" "${_marker}" _done)
if(NOT _done EQUAL -1)
  return()
endif()
string(FIND "${_text}" "${_old}" _at)
if(_at EQUAL -1)
  message(FATAL_ERROR "nasm3_check.cmake: the -Ox check of test_nasm() was not found in ${_file}; "
                      "review third_party/libaom/nasm3_check.cmake against the new libaom pin")
endif()
set(_new
    [[  # OVT nasm3_check: NASM 3.x omits -Ox from `nasm -hf` but always optimises multipass.
  execute_process(COMMAND ${CMAKE_ASM_NASM_COMPILER} -v OUTPUT_VARIABLE ovt_nasm_version)
  if(NOT "${nasm_helptext}" MATCHES "-Ox" AND NOT "${ovt_nasm_version}" MATCHES "NASM version ([3-9]|[1-9][0-9])\\.")]])
string(REPLACE "${_old}" "${_new}" _text "${_text}")
file(WRITE "${_file}" "${_text}")
message(STATUS "nasm3_check.cmake: test_nasm() accepts NASM 3.x")
