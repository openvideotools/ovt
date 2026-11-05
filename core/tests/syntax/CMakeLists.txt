# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the syntax module (chapter 7 §7.3). They link the module objects directly because ovt_core
# exports only the C API (#62).

add_executable(ovt_tests_syntax_user_data test_syntax_user_data.cpp)
target_link_libraries(ovt_tests_syntax_user_data PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_syntax_user_data "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax_user_data)

# SyntaxBuilder and SyntaxNodeBuffer (WI-1.106).
add_executable(ovt_tests_syntax test_syntax_builder.cpp)
target_link_libraries(ovt_tests_syntax PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_syntax "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax)
