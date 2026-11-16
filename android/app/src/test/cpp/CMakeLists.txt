# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host unit test of the platform-independent half of the Android HDR output (WI-0.052): the encoding rule, the
# dataspace field values and the PQ test pattern of android/app/src/main/cpp/hdr_pattern.cpp. Added from
# tests/CMakeLists.txt on non-Android builds; the EGL/GLES half (hdr_surface.cpp) needs a device.

ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
add_executable(ovt_tests_android_hdr test_hdr_pattern.cpp
                                     "${PROJECT_SOURCE_DIR}/android/app/src/main/cpp/hdr_pattern.cpp")
target_include_directories(ovt_tests_android_hdr PRIVATE "${PROJECT_SOURCE_DIR}/android/app/src/main/cpp")
target_link_libraries(ovt_tests_android_hdr PRIVATE ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_android_hdr "GPL-3.0-or-later")
ovt_catch_discover_tests(ovt_tests_android_hdr)
