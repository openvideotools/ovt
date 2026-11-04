# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# vcpkg triplet arm64-android-ovt (chapter 13 §13.2.4, WI-0.001): static libraries absorbed into libovt_core, dynamic
# C runtime, hidden visibility, position-independent code.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
# API level 28 (minSdk); vcpkg has no VCPKG_ANDROID_API variable.
set(VCPKG_CMAKE_SYSTEM_VERSION 28)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DANDROID_ABI=arm64-v8a
  -DANDROID_STL=c++_shared)
set(VCPKG_C_FLAGS "-fvisibility=hidden")
set(VCPKG_CXX_FLAGS "-fvisibility=hidden -fvisibility-inlines-hidden")
