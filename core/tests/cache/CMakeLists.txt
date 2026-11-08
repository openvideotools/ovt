# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the cache module (chapter 06 §6.7, WI-0.091). They link the module objects directly because
# ovt_core exports only the C API, and never ovt::core as well (#62 rule 2).

add_executable(ovt_tests_cache test_cache_meta.cpp)
target_link_libraries(ovt_tests_cache PRIVATE ovt_core_objects ovt::flags ${flatbuffers_TARGET} ${Catch2_TARGET})
target_include_directories(ovt_tests_cache PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_compile_definitions(ovt_tests_cache PRIVATE "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_cache "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_cache)
