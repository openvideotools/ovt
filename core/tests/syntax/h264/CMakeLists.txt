# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the H.264 header parsers (chapter 7 §7.3.4). They link the module objects directly (#62).

# WI-1.108a: NAL header and SPS (VUI, HRD, scaling lists); WI-1.108b adds the PPS tests to the same executable.
add_executable(ovt_tests_syntax_h264_sps_pps test_syntax_h264_sps_pps.cpp)
target_link_libraries(ovt_tests_syntax_h264_sps_pps PRIVATE ovt_core_objects ovt_test_support ovt::flags
                                                            ${Catch2_TARGET})
ovt_set_license(ovt_tests_syntax_h264_sps_pps "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_syntax_h264_sps_pps)
