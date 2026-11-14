# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Test driver of fuzz_canary_trips (WI-0.033): passes only if the canary replay crashes or exits non-zero.
# CTest's WILL_FAIL does not invert a crash ("Subprocess aborted"), so the child runs here instead.
#   cmake -DEXE=<ovt_fuzz_canary_regress> -DDIR=<corpus dir> -P expect_crash.cmake
set(ENV{OVT_FUZZ_CANARY} 1)
execute_process(COMMAND "${EXE}" "${DIR}" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(rc STREQUAL "0")
  message(FATAL_ERROR "the canary did not crash with OVT_FUZZ_CANARY=1: the fuzz lanes could not catch a crash")
endif()
message(STATUS "canary crashed as intended: ${rc}")
