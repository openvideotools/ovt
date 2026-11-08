# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the cache module (chapter 06 §6.7, WI-0.091 and WI-0.092). They link the module objects directly because
# ovt_core exports only the C API, and never ovt::core as well (#62 rule 2).

add_executable(ovt_tests_cache test_cache.cpp test_cache_meta.cpp test_evict.cpp test_invalidate.cpp)
target_link_libraries(ovt_tests_cache PRIVATE ovt_core_objects ovt::flags ${flatbuffers_TARGET} ${Catch2_TARGET})
target_include_directories(ovt_tests_cache PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_compile_definitions(ovt_tests_cache PRIVATE "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_cache "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_cache)

# JSON Schema acceptance tests: the driver runs a test case, then validates the JSON file it leaves behind with
# `python -m jsonschema` (skipped, 77, without the package). WI-0.091 acceptance test 1, second half: the AT1
# meta.json against cache-meta-1.schema.json. WI-0.092 acceptance test 4: cache.json after the eviction of
# acceptance test 3 against cache-json-1.schema.json. Each driver gives its test case a directory of its own.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME cache_meta_json_schema
    COMMAND
      "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/validate_json_schema.py" $<TARGET_FILE:ovt_tests_cache>
      "test_cache: AT1*" "${CMAKE_CURRENT_BINARY_DIR}/schema_at1" "*/meta.json"
      "${PROJECT_SOURCE_DIR}/docs/spec/cache-meta-1.schema.json")
  add_test(
    NAME cache_json_schema
    COMMAND
      "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/validate_json_schema.py" $<TARGET_FILE:ovt_tests_cache>
      "test_evict: AT3*" "${CMAKE_CURRENT_BINARY_DIR}/schema_at3" "cache.json"
      "${PROJECT_SOURCE_DIR}/docs/spec/cache-json-1.schema.json")
  set_tests_properties(cache_meta_json_schema cache_json_schema PROPERTIES LABELS "fast;unit;cache" SKIP_RETURN_CODE
                                                                                                    77)
endif()

# WI-0.091: ovt/cache.h compiles as C11 (chapter 12 §12.3.1 rule 1). A compile-only check, as in core/tests/model.
add_library(ovt_tests_cache_c_api OBJECT c_api_cache_compile.c)
target_link_libraries(ovt_tests_cache_c_api PRIVATE ovt::flags)
target_include_directories(ovt_tests_cache_c_api PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
set_target_properties(ovt_tests_cache_c_api PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
ovt_set_license(ovt_tests_cache_c_api "LGPL-2.1-or-later")
add_dependencies(ovt_tests_cache ovt_tests_cache_c_api)

# WI-0.091 acceptance test 4 and the ovt/cache.h round trip, linked against ovt::core (the exported ABI).
add_executable(ovt_tests_cache_api test_cache_c_api.cpp)
target_link_libraries(ovt_tests_cache_api PRIVATE ovt::core ovt::flags ovt_schemas ${flatbuffers_TARGET}
                                                  ${Catch2_TARGET})
target_include_directories(ovt_tests_cache_api PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_compile_definitions(ovt_tests_cache_api PRIVATE "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_cache_api "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_cache_api)
