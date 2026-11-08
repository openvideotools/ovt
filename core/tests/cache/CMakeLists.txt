# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the cache module (chapter 06 §6.7, WI-0.091). They link the module objects directly because
# ovt_core exports only the C API, and never ovt::core as well (#62 rule 2).

add_executable(ovt_tests_cache test_cache.cpp test_cache_meta.cpp)
target_link_libraries(ovt_tests_cache PRIVATE ovt_core_objects ovt::flags ${flatbuffers_TARGET} ${Catch2_TARGET})
target_include_directories(ovt_tests_cache PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_compile_definitions(ovt_tests_cache PRIVATE "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_cache "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_cache)

# WI-0.091 acceptance test 1, second half: the meta.json that the AT1 test case leaves under at1/ validates
# against docs/spec/cache-meta-1.schema.json with `python -m jsonschema`. The driver runs that test case first,
# so the order of the two CTest tests does not matter. Skipped (77) without the jsonschema package
# (tools/requirements.txt).
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME cache_meta_json_schema
    COMMAND
      "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/validate_meta_schema.py" $<TARGET_FILE:ovt_tests_cache>
      "${CMAKE_CURRENT_BINARY_DIR}/at1" "${PROJECT_SOURCE_DIR}/docs/spec/cache-meta-1.schema.json")
  set_tests_properties(cache_meta_json_schema PROPERTIES LABELS "fast;unit;cache" SKIP_RETURN_CODE 77)
endif()
