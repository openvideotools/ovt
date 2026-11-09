# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# H.265 header parser tests (chapter 7 §7.3.5, WI-1.112). They link the module objects directly because ovt_core
# exports only the C API (#62); ovt_test_support provides corpus_or_skip().

add_executable(ovt_tests_syntax_h265 test_syntax_h265_nal_ptl_hrd.cpp)
target_link_libraries(ovt_tests_syntax_h265 PRIVATE ovt_core_objects ovt_test_support ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_syntax_h265 "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax_h265)
