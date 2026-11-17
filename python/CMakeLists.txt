# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Placeholder created by WI-0.000 (chapter 0 §0.4). The pybind11 module ovt_py arrives with the chapter 12 work items.

# ---- WI-0.095: Python access to the schemas (chapter 06) ----------------------------------------------------
# The flatc-generated namespace package build/<preset>/generated/python/ovt/fb/ is never committed; it is installed
# next to python/ovt/model.py into the wheel's ovt/ package (chapter 13 §13.2.3).
if(OVT_BUILD_PYTHON)
  install(
    DIRECTORY "${OVT_GENERATED_DIR}/python/ovt/fb"
    DESTINATION ovt
    COMPONENT python
    FILES_MATCHING
    PATTERN "*.py")
  install(FILES ovt/model.py DESTINATION ovt COMPONENT python)
endif()

if(OVT_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pytest, numpy, flatbuffers, zstandard"
                    RESULT_VARIABLE _ovt_no_py_model OUTPUT_QUIET ERROR_QUIET)
  else()
    set(_ovt_no_py_model 1)
  endif()
  if(_ovt_no_py_model EQUAL 0)
    # Stage the package as the wheel lays it out: generated ovt/fb + ovt/model.py (+ the generated empty ovt/__init__.py).
    set(_stage "${CMAKE_CURRENT_BINARY_DIR}/stage")
    add_custom_target(
      ovt_py_model_stage ALL
      COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_stage}"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory "${OVT_GENERATED_DIR}/python/ovt" "${_stage}/ovt"
      COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/ovt/model.py" "${_stage}/ovt/model.py"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ovt/model.py"
      COMMENT "Staging the ovt Python package for python/tests"
      VERBATIM)
    add_dependencies(ovt_py_model_stage ovt_schemas)
    add_test(NAME python_model_pytest COMMAND "${Python3_EXECUTABLE}" -m pytest -q -p no:cacheprovider
                                              "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model.py")
    set_tests_properties(
      python_model_pytest PROPERTIES LABELS "fast;python" TIMEOUT 120 ENVIRONMENT
                                     "PYTHONPATH=${_stage};OVT_GOLDEN_DIR=${PROJECT_SOURCE_DIR}/core/tests/golden")
  else()
    message(STATUS "pytest, numpy, flatbuffers or zstandard missing for ${Python3_EXECUTABLE}: python_model_pytest not "
                   "registered (pip install -r tools/requirements-test.txt flatbuffers==25.12.19 numpy zstandard)")
  endif()
  unset(_ovt_no_py_model)
endif()
