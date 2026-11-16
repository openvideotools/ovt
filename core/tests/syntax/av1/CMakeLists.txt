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

# WI-1.117: uncompressed_header() sub-structures (part a: frame size, render size, frame_size_with_refs, tile_info).
add_executable(ovt_tests_syntax_av1_frame_header test_syntax_av1_frame_header.cpp)
target_link_libraries(ovt_tests_syntax_av1_frame_header PRIVATE ovt_core_objects ovt_test_support ovt::flags
                                                                ${Catch2_TARGET})
ovt_set_license(ovt_tests_syntax_av1_frame_header "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax_av1_frame_header)

# WI-1.118: uncompressed_header() rows 20–27 and 29 (loop filter, CDEF, LR, tx mode, reference / skip mode,
# global motion, film grain, tile group).
add_executable(ovt_tests_syntax_av1_frame_header2 test_syntax_av1_frame_header2.cpp)
target_link_libraries(ovt_tests_syntax_av1_frame_header2 PRIVATE ovt_core_objects ovt_test_support ovt::flags
                                                                 ${Catch2_TARGET})
target_include_directories(ovt_tests_syntax_av1_frame_header2 PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
ovt_set_license(ovt_tests_syntax_av1_frame_header2 "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax_av1_frame_header2)
