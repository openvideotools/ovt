# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the container module (chapter 08 §8.10, WI-1.200). They link the module objects directly
# because ovt_core exports only the C API.

add_executable(ovt_tests_container annexb_test.cpp c_api_test.cpp ivf_test.cpp obu_test.cpp sniff_test.cpp y4m_test.cpp)
target_include_directories(ovt_tests_container PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ovt_tests_container PRIVATE ovt_core_objects ovt_test_support ovt_schemas ovt::flags
                                                  ${Catch2_TARGET} ${nlohmann_json_TARGET} ${flatbuffers_TARGET})
target_compile_definitions(ovt_tests_container PRIVATE "OVT_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
                                                       "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_container "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_container)
