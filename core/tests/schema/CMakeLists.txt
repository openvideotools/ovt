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
