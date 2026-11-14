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

# WI-1.403: ViewState -> pl_render_params, target frames, tone/gamut mapping, dithering (§10.3.2, §10.3.4, §10.3.5). The
# acceptance test synthesises tests/corpora/patterns/pq_patches_1000 from its expected.json.
target_sources(ovt_tests_render PRIVATE test_render_params.cpp)

# WI-1.406: NATIVE targets (§10.4.1): encoding choice, CAMetalLayer table, headroom rule, and the swapchain path on a
# headless surface (lavapipe; skipped without VK_EXT_headless_surface). On macOS, test_macos_metal.mm drives a real
# CAMetalLayer through MoltenVK.
target_sources(ovt_tests_render PRIVATE test_native_target.cpp)
if(APPLE)
  target_sources(ovt_tests_render PRIVATE test_macos_metal.mm)
  set_source_files_properties(test_macos_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(ovt_tests_render PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_link_libraries(ovt_tests_render PRIVATE "-framework QuartzCore" "-framework Metal" "-framework CoreGraphics"
                                                 "-framework CoreFoundation")
endif()

# WI-1.406 step 5: the manual EDR tool for acceptance test 1 (needs a display; not registered with ctest).
if(APPLE)
  add_executable(ovt_manual_edr_pattern manual_edr_pattern.cpp manual_edr_window.mm)
  set_source_files_properties(manual_edr_window.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(ovt_manual_edr_pattern PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_link_libraries(ovt_manual_edr_pattern PRIVATE ovt::render ovt::flags ${nlohmann_json_TARGET}
                                                       "-framework AppKit" "-framework QuartzCore")
  target_compile_definitions(ovt_manual_edr_pattern
                             PRIVATE "OVT_PATTERNS_DIR=\"${PROJECT_SOURCE_DIR}/tests/corpora/patterns\"")
  ovt_set_license(ovt_manual_edr_pattern "LGPL-2.1-or-later")
endif()
