# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# WI-1.500 acceptance test 1 on the real binary: `ovt` with no arguments exits 2 and stderr ends with
# "See 'ovt --help'.". Run as: cmake -DOVT_EXE=<path to ovt> -P check_no_args.cmake
execute_process(COMMAND "${OVT_EXE}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
get_filename_component(exe_name "${OVT_EXE}" NAME_WE)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "expected exit 2, got ${rc}; stderr: ${err}")
endif()
if(NOT err MATCHES "See '${exe_name} --help'\\.\n$")
  message(FATAL_ERROR "stderr does not end with \"See '${exe_name} --help'.\": ${err}")
endif()
if(NOT out STREQUAL "")
  message(FATAL_ERROR "stdout is not empty: ${out}")
endif()
