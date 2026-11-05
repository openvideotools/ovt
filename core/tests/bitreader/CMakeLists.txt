# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the bitreader module (chapter 7 §7.2.7, WI-1.102 and WI-1.104). They link the module
# objects directly because ovt_core exports only the C API (#62).

add_executable(ovt_tests_bitreader test_bitreader.cpp test_bitreader_rbsp.cpp)
target_link_libraries(ovt_tests_bitreader PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_bitreader "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_bitreader)
