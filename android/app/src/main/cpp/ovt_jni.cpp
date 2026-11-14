// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// JNI glue of the Android app (chapter 13 §13.4.4, JNI block; WI-0.050). The ovt_jni_* functions are registered on
// org.openvideotools.studio.OvtNative from JNI_OnLoad; they are internal to the app, live in no public header and
// call only the C API of chapter 12. Every body is noexcept: errors are returned, never thrown into the JVM.
// WI-0.050 provides ovt_jni_version; WI-0.051 (open/close), WI-0.052 (surface, headroom) and WI-0.053 (plugin
// paths) add the rest of the block.

#include <android/log.h>
#include <jni.h>

#include <array>
#include <exception>
#include <string>

#include "ovt/version.h"

namespace {

constexpr const char* kLogTag = "OVT";
constexpr const char* kNativeClass = "org/openvideotools/studio/OvtNative";

}  // namespace

extern "C" {

// ovt_version_string() plus build information: {"version":"<x.y.z>","build":<ovt_build_info_json()>}. The version
// string is digits, dots and a suffix without quotes, and the build information is already JSON, so no escaping is
// needed. Returns null (no pending Java exception other than NewStringUTF's own) when memory runs out.
static jstring ovt_jni_version(JNIEnv* env, jclass /*clazz*/) noexcept {
  try {
    const std::string json =
        std::string(R"({"version":")") + ovt_version_string() + R"(","build":)" + ovt_build_info_json() + "}";
    return env->NewStringUTF(json.c_str());
  } catch (const std::exception& error) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ovt_jni_version: %s", error.what());
    return nullptr;
  }
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  jclass native_class = env->FindClass(kNativeClass);
  if (native_class == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ovt_jni: class %s not found", kNativeClass);
    return JNI_ERR;
  }
  // Kotlin name, JNI signature, function (§13.4.4 JNI block).
  const std::array<JNINativeMethod, 1> methods{{
      {"version", "()Ljava/lang/String;", reinterpret_cast<void*>(&ovt_jni_version)},
  }};
  const jint registered = env->RegisterNatives(native_class, methods.data(), static_cast<jint>(methods.size()));
  env->DeleteLocalRef(native_class);
  if (registered != JNI_OK) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ovt_jni: RegisterNatives failed (%d)", registered);
    return JNI_ERR;
  }
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "ovt_jni loaded, core %s", ovt_version_string());
  return JNI_VERSION_1_6;
}

}  // extern "C"
