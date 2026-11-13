# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WI-0.013 acceptance test 2 (chapter 13 §13.2.2, OVT_SYSTEM_LIBAOM): configures the check project twice.
#   1. Against fake_libaom (a libaom without CONFIG_INSPECTION): configure must fail with
#      "libaom was not built with CONFIG_INSPECTION".
#   2. Against the bundled libaom.a of third_party/libaom.cmake (built with it): configure must succeed.
#
# Inputs (-D): OVT_CMAKE_DIR, CHECK_SOURCE_DIR, WORK_DIR, GENERATOR, C_COMPILER, AOM_INCLUDE_DIR, FAKE_LIBRARY,
# REAL_LIBRARY, C_FLAGS, LINK_FLAGS (the sanitizer flags of the build, so instrumented archives link).

foreach(var OVT_CMAKE_DIR CHECK_SOURCE_DIR WORK_DIR GENERATOR C_COMPILER AOM_INCLUDE_DIR FAKE_LIBRARY REAL_LIBRARY)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "run_check.cmake: ${var} is required.")
  endif()
endforeach()

function(configure_check name library out_rc out_log)
  set(dir "${WORK_DIR}/${name}")
  file(REMOVE_RECURSE "${dir}")
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${CHECK_SOURCE_DIR}" -B "${dir}" -G "${GENERATOR}" "-DCMAKE_C_COMPILER=${C_COMPILER}"
      "-DCMAKE_C_FLAGS=${C_FLAGS}" "-DCMAKE_EXE_LINKER_FLAGS=${LINK_FLAGS}" "-DOVT_CMAKE_DIR=${OVT_CMAKE_DIR}"
      "-DAOM_LIBRARY=${library}" "-DAOM_INCLUDE_DIR=${AOM_INCLUDE_DIR}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  set(${out_rc} "${rc}" PARENT_SCOPE)
  set(${out_log} "${out}${err}" PARENT_SCOPE)
endfunction()

set(expected "libaom was not built with CONFIG_INSPECTION")

configure_check(without_inspection "${FAKE_LIBRARY}" rc log)
if(rc EQUAL 0)
  message(FATAL_ERROR "Configure against a libaom without CONFIG_INSPECTION succeeded; it must fail.\n${log}")
endif()
string(FIND "${log}" "${expected}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "Configure failed, but without \"${expected}\":\n${log}")
endif()
message(STATUS "without CONFIG_INSPECTION: configure failed with \"${expected}\" (as required)")

configure_check(with_inspection "${REAL_LIBRARY}" rc log)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Configure against the bundled libaom (CONFIG_INSPECTION=1) failed:\n${log}")
endif()
message(STATUS "with CONFIG_INSPECTION: configure succeeded")
