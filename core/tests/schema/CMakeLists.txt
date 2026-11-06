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
  add_test(
    NAME schema_python_import
    COMMAND
      "${Python3_EXECUTABLE}" -c
      "${_ovt_import_check}; import ovt.fb.FrameAnalysis, ovt.fb.ContainerRoot, ovt.fb.MetricsReport, ovt.fb.smoke.Record"
  )
  set_tests_properties(schema_python_import PROPERTIES ENVIRONMENT "PYTHONPATH=${OVT_GENERATED_DIR}/python" LABELS
                                                       "fast;schema" SKIP_RETURN_CODE 77)
endif()

# ---- WI-0.080: test_model_schema is the executable the WI names. It links the
# generated headers through the ovt_schemas INTERFACE target of cmake/OvtFlatBuffers.cmake (WI-0.003), which
# orders it after code generation and adds ${OVT_GENERATED_DIR} as a system include (the generated header
# does not pass -Wuseless-cast as a plain include).

add_executable(test_model_schema test_model_schema.cpp)
target_link_libraries(test_model_schema PRIVATE ovt_schemas ovt::flags ${flatbuffers_TARGET} ${Catch2_TARGET})
ovt_set_license(test_model_schema "LGPL-2.1-or-later")
ovt_catch_discover_tests(test_model_schema)

# WI-0.080 acceptance test 1: the C++, Python and Kotlin outputs of the ovt_schemas target exist at the
# chapter 13 §13.2.3 paths. A CTest test rather than a manual record, so every platform checks its own tree.
add_test(NAME test_model_schema_generated_outputs
         COMMAND "${CMAKE_COMMAND}" -D "OVT_GENERATED_DIR=${OVT_GENERATED_DIR}" -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/check_generated_outputs.cmake")
set_tests_properties(test_model_schema_generated_outputs PROPERTIES LABELS "fast;unit;schema")

# ---- WI-0.081: test_container_schema. Acceptance test 2 compares the §6.4 table list with the compiled
# .bfbs, so this test compiles both schemas to binary schemas with the same flatc that ovt_schemas uses.
# The files stay in this test's build directory: embedding .bfbs in ovt_core is WI-0.083's job
# (cmake/OvtFlatBuffers.cmake), not this test's.

set(_ovt_schema_dir "${PROJECT_SOURCE_DIR}/core/schema")
set(_ovt_test_bfbs "")
foreach(_stem IN ITEMS ovt_model ovt_container)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bfbs/${_stem}.bfbs"
    COMMAND "${OVT_FLATC_EXECUTABLE}" --binary --schema --reflect-names -I "${_ovt_schema_dir}" -o
            "${CMAKE_CURRENT_BINARY_DIR}/bfbs" "${_ovt_schema_dir}/${_stem}.fbs"
    DEPENDS "${_ovt_schema_dir}/ovt_model.fbs" "${_ovt_schema_dir}/ovt_container.fbs"
    COMMENT "flatc: ${_stem}.fbs -> ${_stem}.bfbs (test_container_schema)"
    VERBATIM)
  list(APPEND _ovt_test_bfbs "${CMAKE_CURRENT_BINARY_DIR}/bfbs/${_stem}.bfbs")
endforeach()
add_custom_target(test_container_schema_bfbs DEPENDS ${_ovt_test_bfbs})

add_executable(test_container_schema test_container_schema.cpp)
add_dependencies(test_container_schema test_container_schema_bfbs)
target_link_libraries(test_container_schema PRIVATE ovt_schemas ovt::flags ${flatbuffers_TARGET} ${Catch2_TARGET})
target_compile_definitions(
  test_container_schema PRIVATE "OVT_MODEL_BFBS_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/bfbs/ovt_model.bfbs\""
                                "OVT_CONTAINER_BFBS_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/bfbs/ovt_container.bfbs\"")
ovt_set_license(test_container_schema "LGPL-2.1-or-later")
ovt_catch_discover_tests(test_container_schema)

# ---- WI-0.082: test_metrics_schema. A MetricsReport round trip through the generated C++ code of
# ovt_metrics.fbs (which includes ovt_model.fbs), linked like test_model_schema.

add_executable(test_metrics_schema test_metrics_schema.cpp)
target_link_libraries(test_metrics_schema PRIVATE ovt_schemas ovt::flags ${flatbuffers_TARGET} ${Catch2_TARGET})
ovt_set_license(test_metrics_schema "LGPL-2.1-or-later")
ovt_catch_discover_tests(test_metrics_schema)
