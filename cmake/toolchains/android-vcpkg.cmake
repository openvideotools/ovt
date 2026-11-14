# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Android builds through the Qt Gradle plugin (android/, WI-0.050): Qt's qt.toolchain.cmake chainloads this file,
# which picks the vcpkg triplet from ANDROID_ABI and then loads vcpkg.cmake (which chainloads the NDK toolchain).
# The Qt toolchain sets ANDROID_ABI before it chainloads, also in the secondary-ABI builds of
# QT_ANDROID_BUILD_ALL_ABIS, which Qt configures without passing ANDROID_ABI on the command line. Without this step
# vcpkg would guess the triplet from the host (x64-android) for every ABI. Preset builds set the triplet already.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(VCPKG_TARGET_TRIPLET "arm64-android-ovt" CACHE STRING "vcpkg triplet (derived from ANDROID_ABI)")
  elseif(ANDROID_ABI STREQUAL "x86_64")
    set(VCPKG_TARGET_TRIPLET "x64-android-ovt" CACHE STRING "vcpkg triplet (derived from ANDROID_ABI)")
  else()
    message(FATAL_ERROR "Unsupported ANDROID_ABI '${ANDROID_ABI}' (arm64-v8a, x86_64; chapter 13 §13.4.7).")
  endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../third_party/vcpkg/scripts/buildsystems/vcpkg.cmake")
