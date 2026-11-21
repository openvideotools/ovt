# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WI-1.130: tests of ovt_stream_unit_bytes (ovt/index.h). They link the module objects because ovt_core exports only
# the C API (#62).
add_executable(ovt_tests_api_stream test_api_stream_api.cpp)
target_link_libraries(ovt_tests_api_stream PRIVATE ovt_core_objects ovt_test_support ovt::flags ${Catch2_TARGET})
target_include_directories(ovt_tests_api_stream PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_compile_definitions(ovt_tests_api_stream
                           PRIVATE OVT_INDEX_BUILDER_SEEDS="${PROJECT_SOURCE_DIR}/core/tests/fuzz/corpus/index_builder")
ovt_set_license(ovt_tests_api_stream "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_api_stream)
