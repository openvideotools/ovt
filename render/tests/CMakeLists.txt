# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Tests of libovt-render. Today only the third-party probe of WI-0.017; the render tests arrive with chapter 10.

# WI-0.017: tp::moltenvk (macOS prebuilt) links directly, as ovt_render will (§13.5.6).
if(TARGET tp::moltenvk)
  ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
  add_executable(ovt_tests_moltenvk_probe test_moltenvk_probe.cpp)
  target_link_libraries(ovt_tests_moltenvk_probe PRIVATE tp::moltenvk ovt::flags ${Catch2_TARGET})
  target_compile_definitions(ovt_tests_moltenvk_probe PRIVATE "OVT_MOLTENVK_DYLIB=\"$<TARGET_FILE:tp::moltenvk>\"")
  ovt_set_license(ovt_tests_moltenvk_probe "LGPL-2.1-or-later")
  ovt_catch_discover_tests(ovt_tests_moltenvk_probe)
endif()
