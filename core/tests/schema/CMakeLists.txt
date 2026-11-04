# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WI-0.003: build -> verify -> JSON -> build on a test-only schema, through ovt_add_flatbuffers_schema.

ovt_add_flatbuffers_schema(ovt_schema_smoke "${CMAKE_CURRENT_SOURCE_DIR}/smoke.fbs")

add_executable(ovt_tests_schema_smoke test_schema_smoke.cpp)
target_link_libraries(ovt_tests_schema_smoke PRIVATE ovt_schema_smoke ovt::flags ${Catch2_TARGET} ${flatbuffers_TARGET})
target_compile_definitions(ovt_tests_schema_smoke PRIVATE
  "OVT_SMOKE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
  "OVT_SMOKE_FBS_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/smoke.fbs\"")
ovt_set_license(ovt_tests_schema_smoke "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_schema_smoke)

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
