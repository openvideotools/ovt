// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// JNI glue of the Android app (chapter 13 §13.4.4, JNI block; WI-0.050). The ovt_jni_* functions are registered on
// org.openvideotools.studio.OvtNative from JNI_OnLoad; they are internal to the app, live in no public header and
// call only the C API of chapter 12. Every body is noexcept: errors are returned, never thrown into the JVM.
// WI-0.050 provides ovt_jni_version; WI-0.051 adds open_fd/close (plus ovt_jni_info_json and ovt_jni_file_size,
// additive helpers of the debug overlay and of the 64-bit size check); WI-0.052 (surface, headroom) and WI-0.053
// (plugin paths) add the rest of the block.

#include <android/log.h>
#include <jni.h>

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#include "ovt/container.h"
#include "ovt/io.h"
#include "ovt/status.h"
#include "ovt/version.h"

namespace {

constexpr const char* kLogTag = "OVT";
constexpr const char* kNativeClass = "org/openvideotools/studio/OvtNative";

// One opened file: the descriptor-backed ovt_File and the container sniffed on it (the container must be closed
// first, ovt_file_close() documents the order). The jlong handed to Kotlin is a pointer to this.
struct Session {
  ovt_File* file = nullptr;
  ovt_Container* container = nullptr;
};

Session* session_from(jlong handle) noexcept {
  return reinterpret_cast<Session*>(handle);
}

// RAII for GetStringUTFChars so that every return path releases the characters.
class Utf8 {
 public:
  Utf8(JNIEnv* env, jstring value) : env_(env), value_(value) {
    if (value != nullptr) {
      chars_ = env->GetStringUTFChars(value, nullptr);
    }
  }
  ~Utf8() {
    if (chars_ != nullptr) {
      env_->ReleaseStringUTFChars(value_, chars_);
    }
  }
  Utf8(const Utf8&) = delete;
  Utf8& operator=(const Utf8&) = delete;
  Utf8(Utf8&&) = delete;
  Utf8& operator=(Utf8&&) = delete;
  [[nodiscard]] const char* get() const noexcept { return chars_; }

 private:
  JNIEnv* env_;
  jstring value_;
  const char* chars_ = nullptr;
};

// JSON string body (without quotes) of @p text: quotes, backslashes and control characters escaped.
std::string json_escape(const char* text) {
  std::string out;
  for (const char* p = text != nullptr ? text : ""; *p != '\0'; ++p) {
    const auto c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      static constexpr const char* kHex = "0123456789abcdef";
      out += "\\u00";
      out += kHex[c >> 4U];
      out += kHex[c & 0xFU];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

void log_failure(const char* what, ovt_status status) {
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s: %s, last error %s", what, ovt_status_string(status),
                      ovt_last_error_json());
}

void close_session(Session* session) noexcept {
  if (session == nullptr) {
    return;
  }
  ovt_container_close(session->container);  // NULL is a no-op
  ovt_file_close(session->file);            // closes the descriptor adopted in ovt_jni_open_fd
  delete session;
}

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

// ovt_file_open_fd(fd, size, take_ownership = 1, name) + ovt_container_open (§13.4.4 item 3). The descriptor comes
// from ParcelFileDescriptor.detachFd(), so ownership passes here at the call: it is closed on every failure path
// too (ovt_file_open_fd documents that), and by ovt_jni_close() on success. Returns 0 on error.
static jlong ovt_jni_open_fd(JNIEnv* env, jclass /*clazz*/, jint fd, jlong size, jstring display_name) noexcept {
  try {
    const Utf8 name(env, display_name);
    // A negative size (ParcelFileDescriptor.getStatSize() of -1) means unknown: 0 lets the library fstat.
    const uint64_t known_size = size > 0 ? static_cast<uint64_t>(size) : 0;
    auto session = std::unique_ptr<Session, decltype(&close_session)>(new Session(), &close_session);
    ovt_status status = ovt_file_open_fd(fd, known_size, /*take_ownership=*/1, name.get(), &session->file);
    if (status != OVT_OK) {
      log_failure("ovt_file_open_fd", status);
      return 0;
    }
    // Acceptance test 1 of WI-0.051 greps for exactly this line.
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "opened fd size=%llu",
                        static_cast<unsigned long long>(ovt_file_size(session->file)));
    status = ovt_container_open(session->file, &session->container);
    if (status != OVT_OK) {
      log_failure("ovt_container_open", status);
      return 0;
    }
    return reinterpret_cast<jlong>(session.release());
  } catch (const std::exception& error) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ovt_jni_open_fd: %s", error.what());
    return 0;
  }
}

static void ovt_jni_close(JNIEnv* /*env*/, jclass /*clazz*/, jlong session) noexcept {
  close_session(session_from(session));
}

// Size of the opened file in bytes as ovt_file_size() reports it (64-bit; acceptance test 2); 0 for no session.
static jlong ovt_jni_file_size(JNIEnv* /*env*/, jclass /*clazz*/, jlong session) noexcept {
  const Session* s = session_from(session);
  return s == nullptr ? 0 : static_cast<jlong>(ovt_file_size(s->file));
}

// The debug overlay's JSON: the file, the sniffed container format and one entry per track. SPEC-ASSUMPTION(#417):
// the FlatBuffer-to-JSON printer behind `ovt info` (WI-1.500) does not exist yet, so a track entry carries the
// size of its TrackInfo FlatBuffer (ovt_container_track_info) instead of the decoded fields; WI-1.500 can replace
// this with the printer's output without changing the JNI signature.
static jstring ovt_jni_info_json(JNIEnv* env, jclass /*clazz*/, jlong session) noexcept {
  const Session* s = session_from(session);
  if (s == nullptr) {
    return nullptr;
  }
  try {
    std::string json = std::string(R"({"name":")") + json_escape(ovt_file_display_name(s->file)) + R"(","size":)" +
                       std::to_string(ovt_file_size(s->file)) + R"(,"format":")" +
                       json_escape(ovt_container_format(s->container)) + R"(","track_count":)";
    const int count = ovt_container_track_count(s->container);
    json += std::to_string(count) + R"(,"tracks":[)";
    for (int i = 0; i < count; ++i) {
      ovt_Buffer buffer{};
      const ovt_status status = ovt_container_track_info(s->container, i, &buffer);
      json += (i > 0 ? "," : "") + std::string(R"({"index":)") + std::to_string(i);
      if (status == OVT_OK) {
        json += R"(,"track_info_bytes":)" + std::to_string(buffer.size);
      } else {
        json += R"(,"error":")" + json_escape(ovt_status_string(status)) + "\"";
      }
      json += "}";
      ovt_buffer_free(&buffer);
    }
    json += "]}";
    return env->NewStringUTF(json.c_str());
  } catch (const std::exception& error) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ovt_jni_info_json: %s", error.what());
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
  const std::array<JNINativeMethod, 5> methods{{
      {"version", "()Ljava/lang/String;", reinterpret_cast<void*>(&ovt_jni_version)},
      {"openFd", "(IJLjava/lang/String;)J", reinterpret_cast<void*>(&ovt_jni_open_fd)},
      {"close", "(J)V", reinterpret_cast<void*>(&ovt_jni_close)},
      {"fileSize", "(J)J", reinterpret_cast<void*>(&ovt_jni_file_size)},
      {"infoJson", "(J)Ljava/lang/String;", reinterpret_cast<void*>(&ovt_jni_info_json)},
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
