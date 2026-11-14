// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// JNI glue of the Android app (chapter 13 §13.4.4, JNI block; WI-0.050). The ovt_jni_* functions are registered on
// org.openvideotools.studio.OvtNative from JNI_OnLoad; they are internal to the app, live in no public header and
// call only the C API of chapter 12. Every body is noexcept: errors are returned, never thrown into the JVM.
// WI-0.050 provides ovt_jni_version; WI-0.051 (open/close), WI-0.052 (surface, headroom, display) and WI-0.053
// (plugin paths) add the rest of the block.

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <array>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "hdr_surface.h"
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

// WI-0.052: HdrSurfaceView's Surface → ANativeWindow → dataspace, EGL surface and PQ test pattern (hdr_surface.cpp),
// which logs `OVT_HDR dataspace=<name>` whenever it sets the dataspace. `session` is unused until WI-0.051 brings
// sessions: phase 0 has one picture surface per process.
static jboolean ovt_jni_surface_attach(JNIEnv* env, jclass /*clazz*/, jlong /*session*/, jobject surface) noexcept {
  if (surface == nullptr) {
    return JNI_FALSE;
  }
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);  // one reference, handed to hdr_surface_attach
  return ovt::android_shell::hdr_surface_attach(window) ? JNI_TRUE : JNI_FALSE;
}

static void ovt_jni_surface_detach(JNIEnv* /*env*/, jclass /*clazz*/, jlong /*session*/) noexcept {
  ovt::android_shell::hdr_surface_detach();
}

static void ovt_jni_set_headroom(JNIEnv* /*env*/, jclass /*clazz*/, jlong /*session*/, jfloat hdr_sdr_ratio) noexcept {
  ovt::android_shell::hdr_set_headroom(hdr_sdr_ratio);
}

// SPEC-ASSUMPTION(#436): the JNI block of §13.4.4 has no way to tell native code which HDR types the display has,
// which the §10.4.3 encoding rule needs. This entry takes the arguments of WI-4.458's nativeSetDisplayCaps, which
// later moves the body into libovt-render.
static void ovt_jni_set_display_caps(JNIEnv* env, jclass /*clazz*/, jlong /*session*/, jintArray hdr_types,
                                     jfloat max_nits, jfloat min_nits, jfloat max_fall_nits, jfloat hdr_sdr_ratio,
                                     jint api_level) noexcept {
  try {
    std::vector<std::int32_t> types;
    if (hdr_types != nullptr) {
      types.resize(static_cast<std::size_t>(env->GetArrayLength(hdr_types)));
      env->GetIntArrayRegion(hdr_types, 0, static_cast<jsize>(types.size()), types.data());
    }
    ovt::android_shell::hdr_set_display(types, {max_nits, min_nits, max_fall_nits, hdr_sdr_ratio, api_level});
  } catch (const std::exception& error) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ovt_jni_set_display_caps: %s", error.what());
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
  const std::array<JNINativeMethod, 5> methods{{
      {"version", "()Ljava/lang/String;", reinterpret_cast<void*>(&ovt_jni_version)},
      {"surfaceAttach", "(JLandroid/view/Surface;)Z", reinterpret_cast<void*>(&ovt_jni_surface_attach)},
      {"surfaceDetach", "(J)V", reinterpret_cast<void*>(&ovt_jni_surface_detach)},
      {"setHeadroom", "(JF)V", reinterpret_cast<void*>(&ovt_jni_set_headroom)},
      {"setDisplayCaps", "(J[IFFFFI)V", reinterpret_cast<void*>(&ovt_jni_set_display_caps)},
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
