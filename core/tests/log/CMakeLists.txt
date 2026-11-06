# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the log module (chapter 7 §7.1.6, chapter 12 §12.3.4; WI-1.512). They link the module
# objects directly because ovt_core exports only the C API, and never ovt::core as well (#62 rule 2).

add_executable(ovt_tests_log test_log.cpp)
target_link_libraries(ovt_tests_log PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_log "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_log)
