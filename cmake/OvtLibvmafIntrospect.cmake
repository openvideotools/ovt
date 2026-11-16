# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Checks a configured libvmaf Meson build against chapter 9 §9.5.1 (WI-3.309 step 1, acceptance test 3). Script
# mode; third_party/libvmaf.cmake runs it between the configure and build steps of tp_libvmaf, and
# core/tests/metrics runs it as a test:
#
#   cmake -DOVT_MESON=<meson> -DBUILD_DIR=<meson build dir> -DEXPECT=<name=value|...> -DREQUIRE_NEON=<ON|OFF>
#         -P cmake/OvtLibvmafIntrospect.cmake
#   cmake -DOPTIONS_JSON=<file> -DTARGETS_JSON=<file> -DEXPECT=... -DREQUIRE_NEON=... -P ...   (test hook)
#
# `meson introspect --buildoptions` must give every EXPECT option its value (booleans compare as true/false), and
# with REQUIRE_NEON=ON `meson introspect --targets` must list `vif_neon.c`: on arm64 libvmaf compiles its NEON paths
# by itself, and a build without them is several times slower. Fails (FATAL_ERROR) naming what differs.

cmake_minimum_required(VERSION 3.25)

foreach(var EXPECT REQUIRE_NEON)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "OvtLibvmafIntrospect: -D${var}=... is required")
  endif()
endforeach()

# Reads `meson introspect <flag>` of BUILD_DIR, or the given file.
function(_ovt_vmaf_introspect flag file out)
  if(file)
    file(READ "${file}" json)
  else()
    if(NOT OVT_MESON OR NOT BUILD_DIR)
      message(FATAL_ERROR "OvtLibvmafIntrospect: give OVT_MESON and BUILD_DIR, or the JSON files")
    endif()
    execute_process(COMMAND "${OVT_MESON}" introspect ${flag} "${BUILD_DIR}" OUTPUT_VARIABLE json ERROR_VARIABLE err
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "OvtLibvmafIntrospect: `meson introspect ${flag} ${BUILD_DIR}` failed (${rc}): ${err}")
    endif()
  endif()
  set(${out} "${json}" PARENT_SCOPE)
endfunction()

function(_ovt_vmaf_bool value out)
  string(TOLOWER "${value}" v)
  if(v STREQUAL "on" OR v STREQUAL "true")
    set(v true)
  elseif(v STREQUAL "off" OR v STREQUAL "false")
    set(v false)
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

_ovt_vmaf_introspect(--buildoptions "${OPTIONS_JSON}" options)
string(JSON n ERROR_VARIABLE jerr LENGTH "${options}")
if(jerr)
  message(FATAL_ERROR "OvtLibvmafIntrospect: --buildoptions is not a JSON array: ${jerr}")
endif()
set(actual "")
if(n GREATER 0)
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON name GET "${options}" ${i} name)
    string(JSON value GET "${options}" ${i} value)
    _ovt_vmaf_bool("${value}" value)
    set(actual_${name} "${value}")
    list(APPEND actual "${name}")
  endforeach()
endif()

string(REPLACE "|" ";" expect "${EXPECT}")
set(problems "")
foreach(entry IN LISTS expect)
  if(NOT entry MATCHES "^([a-z0-9_]+)=(.*)$")
    message(FATAL_ERROR "OvtLibvmafIntrospect: EXPECT entry '${entry}' is not name=value")
  endif()
  set(name "${CMAKE_MATCH_1}")
  _ovt_vmaf_bool("${CMAKE_MATCH_2}" want)
  if(NOT name IN_LIST actual)
    list(APPEND problems "option ${name} is missing (libvmaf has no such option)")
  elseif(NOT actual_${name} STREQUAL want)
    list(APPEND problems "option ${name} is '${actual_${name}}', chapter 9 §9.5.1 needs '${want}'")
  endif()
endforeach()

if(REQUIRE_NEON)
  _ovt_vmaf_introspect(--targets "${TARGETS_JSON}" targets)
  string(FIND "${targets}" "vif_neon.c" pos)
  if(pos EQUAL -1)
    list(APPEND problems "the arm64 build does not compile vif_neon.c (the NEON paths of libvmaf)")
  endif()
endif()

if(problems)
  list(JOIN problems "\n  " text)
  message(FATAL_ERROR "libvmaf Meson build differs from chapter 9 §9.5.1:\n  ${text}")
endif()
message(STATUS "libvmaf Meson build matches chapter 9 §9.5.1 (NEON required: ${REQUIRE_NEON})")
