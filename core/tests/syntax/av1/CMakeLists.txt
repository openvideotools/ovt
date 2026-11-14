# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# AV1 syntax parser tests (chapter 7 §7.3.6). They link the module objects directly (#62).

# WI-1.116: OBU header, sequence header, temporal delimiter, metadata, padding, tile list.
add_executable(ovt_tests_syntax_av1_sequence_metadata test_syntax_av1_sequence_metadata.cpp)
target_link_libraries(ovt_tests_syntax_av1_sequence_metadata PRIVATE ovt_core_objects ovt_test_support ovt::flags
                                                                     ${Catch2_TARGET})
ovt_set_license(ovt_tests_syntax_av1_sequence_metadata "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax_av1_sequence_metadata)
