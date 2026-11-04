# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of libovt-core. WI-0.002 adds the first Catch2 test; WI-0.550 owns this file and adds the
# framework (golden runner, test_support, tag-to-label mapping).

ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
include(Catch)

add_executable(ovt_tests_core test_version.cpp)
target_link_libraries(ovt_tests_core PRIVATE ovt::core ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_core "LGPL-2.1-or-later")
catch_discover_tests(ovt_tests_core DISCOVERY_MODE PRE_TEST)
