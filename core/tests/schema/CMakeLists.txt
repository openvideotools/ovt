# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WI-0.003: build -> verify -> JSON -> build on a test-only schema, through ovt_add_flatbuffers_schema.

ovt_add_flatbuffers_schema(ovt_schema_smoke "${CMAKE_CURRENT_SOURCE_DIR}/smoke.fbs")

add_executable(ovt_tests_schema_smoke test_schema_smoke.cpp)
target_link_libraries(ovt_tests_schema_smoke PRIVATE ovt_schema_smoke ovt::flags ${Catch2_TARGET})
target_compile_definitions(
  ovt_tests_schema_smoke PRIVATE "OVT_SMOKE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
                                 "OVT_SMOKE_FBS_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/smoke.fbs\"")
ovt_set_license(ovt_tests_schema_smoke "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_schema_smoke)

# Acceptance test 1 of WI-0.003: the generated Python package imports (namespace layout, chapter 13 §13.2.3).
# It needs the flatbuffers Python runtime (tools/requirements-test.txt); without it the test is skipped (77).
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(_ovt_import_check
      "import importlib.util, sys; sys.exit(77) if importlib.util.find_spec('flatbuffers') is None else None")
  add_test(NAME schema_python_import COMMAND "${Python3_EXECUTABLE}" -c
                                             "${_ovt_import_check}; import ovt.fb.FrameAnalysis, ovt.fb.smoke.Record")
  set_tests_properties(schema_python_import PROPERTIES ENVIRONMENT "PYTHONPATH=${OVT_GENERATED_DIR}/python" LABELS
                                                       "fast;schema" SKIP_RETURN_CODE 77)
endif()
