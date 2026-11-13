# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# JNI glue of the Android app (chapter 13 §13.4.4; WI-0.050): libovt_jni.so, loaded by OvtNative.kt and registered
# with RegisterNatives. Built inside the repository's own CMake project, from app/CMakeLists.txt when ANDROID, because
# the Qt Gradle plugin configures the repository root (SPEC-ASSUMPTION(#355): §13.4.1 describes this file
# as a top-level project that adds the repository as a subdirectory, which the root's CMAKE_SOURCE_DIR uses forbid).
# app/CMakeLists.txt also lists the library in QT_ANDROID_EXTRA_LIBS so that androiddeployqt packages it.

add_library(ovt_jni SHARED ovt_jni.cpp)
# The C API only (ovt/version.h); `log` is the NDK's liblog for __android_log_print.
target_link_libraries(ovt_jni PRIVATE ovt::core ovt::flags log)
ovt_set_license(ovt_jni "GPL-3.0-or-later")
