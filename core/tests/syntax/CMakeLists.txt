# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the syntax module (chapter 7 §7.3). They link the module objects directly because ovt_core
# exports only the C API (#62).

add_executable(ovt_tests_syntax_user_data test_syntax_user_data.cpp)
target_link_libraries(ovt_tests_syntax_user_data PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_syntax_user_data "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax_user_data)

add_executable(ovt_tests_syntax_unit_scan test_syntax_unit_scan.cpp)
target_link_libraries(ovt_tests_syntax_unit_scan PRIVATE ovt_core_objects ovt_test_support ovt::flags ${Catch2_TARGET})
target_compile_definitions(ovt_tests_syntax_unit_scan PRIVATE "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_syntax_unit_scan "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax_unit_scan)
