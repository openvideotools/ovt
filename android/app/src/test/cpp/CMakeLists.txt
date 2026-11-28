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

# WI-A.403: the shared EGL context and fence of EMBEDDED mode (android/app/src/main/cpp/gl_share.cpp) on the host's
# EGL and GLES 3 (Mesa on Linux; the cases skip without a display). Not built where the host has no EGL (macOS).
find_library(OVT_HOST_EGL_LIBRARY NAMES EGL)
find_library(OVT_HOST_GLES_LIBRARY NAMES GLESv2)
find_path(OVT_HOST_EGL_INCLUDE NAMES EGL/egl.h)
if(OVT_HOST_EGL_LIBRARY AND OVT_HOST_GLES_LIBRARY AND OVT_HOST_EGL_INCLUDE)
  add_executable(ovt_tests_android_gl_share test_gl_share.cpp
                                            "${PROJECT_SOURCE_DIR}/android/app/src/main/cpp/gl_share.cpp")
  target_include_directories(ovt_tests_android_gl_share PRIVATE "${PROJECT_SOURCE_DIR}/android/app/src/main/cpp"
                                                                "${OVT_HOST_EGL_INCLUDE}")
  target_link_libraries(ovt_tests_android_gl_share PRIVATE ovt::flags ${Catch2_TARGET} "${OVT_HOST_EGL_LIBRARY}"
                                                           "${OVT_HOST_GLES_LIBRARY}")
  ovt_set_license(ovt_tests_android_gl_share "GPL-3.0-or-later")
  # libglvnd / Mesa on Linux (MIT), linked by path; the licence gate needs their description (chapter 15).
  foreach(lib IN ITEMS "${OVT_HOST_EGL_LIBRARY}" "${OVT_HOST_GLES_LIBRARY}")
    ovt_set_dependency_license("${lib}" SOURCE system SPDX "MIT" LINKAGE shared)
  endforeach()
  ovt_catch_discover_tests(ovt_tests_android_gl_share)
endif()
