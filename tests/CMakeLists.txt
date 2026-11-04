# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Cross-module tests (chapter 14 §14.2.2, WI-0.550). Module unit tests live next to their module
# (core/tests/, render/tests/, …); this directory holds what spans modules. Fuzz targets live in
# core/tests/fuzz/, not here (chapter 0 §0.4, D14).

# WI-0.004: deliberate heap overflow in ASan builds, label `canary` (run with `ctest -L canary`).
ovt_add_sanitizer_canary()

# SPEC-ASSUMPTION(#11): pytest for the Python tools (chapter 14 §14.2.1) is one CTest test with label `fast`, not a CI job,
# so the `pr` lane runs it wherever pytest is installed (tools/requirements-test.txt). Registered only when
# pytest imports.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pytest"
                  RESULT_VARIABLE _ovt_no_pytest OUTPUT_QUIET ERROR_QUIET)
  if(_ovt_no_pytest EQUAL 0)
    add_test(NAME tools_pytest
             COMMAND "${Python3_EXECUTABLE}" -m pytest -q -p no:cacheprovider "${PROJECT_SOURCE_DIR}/tools/tests"
             WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(tools_pytest PROPERTIES LABELS "fast;tools")
  else()
    message(STATUS "pytest not found for ${Python3_EXECUTABLE}: tools/tests not registered "
                   "(pip install -r tools/requirements-test.txt)")
  endif()
  unset(_ovt_no_pytest)
endif()

# Golden glue (WI-0.556) and the perf harness (WI-0.560) add their directories when they land.
foreach(_dir golden perf downstream)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()
unset(_dir)
