# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Tests of libovt-render: the third-party probes of WI-0.015/WI-0.017 and the render tests of chapter 10
# ("render tests" = ctest -R render_, §10.11; every chapter 10 work item adds its tests here).

# WI-0.017: tp::moltenvk (macOS prebuilt) links directly, as ovt_render will (§13.5.6).
if(TARGET tp::moltenvk)
  ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
  add_executable(ovt_tests_moltenvk_probe test_moltenvk_probe.cpp)
  target_link_libraries(ovt_tests_moltenvk_probe PRIVATE tp::moltenvk ovt::flags ${Catch2_TARGET})
  target_compile_definitions(ovt_tests_moltenvk_probe PRIVATE "OVT_MOLTENVK_DYLIB=\"$<TARGET_FILE:tp::moltenvk>\"")
  ovt_set_license(ovt_tests_moltenvk_probe "LGPL-2.1-or-later")
  ovt_catch_discover_tests(ovt_tests_moltenvk_probe)
endif()

# WI-0.015: tp::placebo (Meson wrapper) links into a test that creates a pl_log.
if(TARGET tp::placebo)
  ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
  add_executable(ovt_tests_placebo_probe test_placebo_probe.cpp)
  target_link_libraries(ovt_tests_placebo_probe PRIVATE tp::placebo ovt::flags ${Catch2_TARGET})
  ovt_set_license(ovt_tests_placebo_probe "LGPL-2.1-or-later")
  # Under ASan, the slow unwinder records the full allocation stack through the stripped Vulkan ICD, so the narrow
  # tests/asan.supp entry for lavapipe's enumeration leak can match its loader frame (#207). Only this binary pays for it.
  set(placebo_env "")
  if(OVT_ENABLE_SANITIZERS AND OVT_SANITIZER STREQUAL "asan-ubsan")
    set(placebo_env ENVIRONMENT_MODIFICATION "LSAN_OPTIONS=string_append::fast_unwind_on_malloc=0")
  endif()
  ovt_catch_discover_tests(ovt_tests_placebo_probe ${placebo_env})
endif()

# WI-1.400: VideoSurface on an OffscreenTexture target (acceptance tests 1 and 2: lavapipe on Linux, MoltenVK on
# macOS). Links the shared libovt_render; the download helper is an exported internal symbol (src/surface/), so the
# test also sees render/src.
ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
add_executable(ovt_tests_render test_h273_map.cpp test_init.cpp test_overlay_builder.cpp fixtures/make_synthetic.cpp)
target_link_libraries(ovt_tests_render PRIVATE ovt::render ovt::flags ${Catch2_TARGET})
target_include_directories(ovt_tests_render PRIVATE "${PROJECT_SOURCE_DIR}/render/src" "${CMAKE_CURRENT_SOURCE_DIR}")
# WI-1.402: the H.273 map (src/color/, exported internal symbols) speaks libplacebo's colour types, so the test also
# needs libplacebo's headers; it calls only pl_color_*_guess itself, everything else goes through libovt_render.
target_link_libraries(ovt_tests_render PRIVATE tp::placebo)
ovt_set_license(ovt_tests_render "LGPL-2.1-or-later")
# Each VideoSurface creates a Vulkan instance and meets the same lavapipe enumeration leak as the probe (#207), so the
# same per-binary slow unwinder lets the tests/asan.supp entry match.
set(render_env "")
if(OVT_ENABLE_SANITIZERS AND OVT_SANITIZER STREQUAL "asan-ubsan")
  set(render_env ENVIRONMENT_MODIFICATION "LSAN_OPTIONS=string_append::fast_unwind_on_malloc=0")
endif()
ovt_catch_discover_tests(ovt_tests_render ${render_env})

# WI-1.401: plane upload and pl_frame construction (§10.3.1), in the same binary as the init test. The siting test
# reads tests/corpora/patterns/siting_checker.expected.json (WI-1.413).
ovt_find_package(nlohmann_json SPDX "MIT" TARGETS nlohmann_json::nlohmann_json HEADER_ONLY)
target_sources(ovt_tests_render PRIVATE test_plane_fill.cpp)
target_link_libraries(ovt_tests_render PRIVATE ${nlohmann_json_TARGET})
target_compile_definitions(ovt_tests_render PRIVATE "OVT_PATTERNS_DIR=\"${PROJECT_SOURCE_DIR}/tests/corpora/patterns\"")
