// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HdrSurfaceView's native side (WI-0.052); see hdr_surface.h. Plain EGL + GLES: the pattern is a list of solid
// rectangles drawn as scissored clears, so no shader is compiled. libplacebo arrives with WI-A.401.

#include "hdr_surface.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/data_space.h>
#include <android/log.h>

#include <array>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

// EGL_EXT_gl_colorspace_bt2020_pq / _hlg tokens, for NDK headers that predate them.
#ifndef EGL_GL_COLORSPACE_KHR
#define EGL_GL_COLORSPACE_KHR 0x309D
#endif
#ifndef EGL_GL_COLORSPACE_BT2020_PQ_EXT
#define EGL_GL_COLORSPACE_BT2020_PQ_EXT 0x3340
#endif
#ifndef EGL_GL_COLORSPACE_BT2020_HLG_EXT
#define EGL_GL_COLORSPACE_BT2020_HLG_EXT 0x3540
#endif

namespace ovt::android_shell {

// hdr_pattern.h spells the ADataSpace fields as numbers so that the host test can use them; they must be the NDK's.
static_assert(kStandardBt709 == ADATASPACE_STANDARD_BT709);
static_assert(kStandardBt2020 == ADATASPACE_STANDARD_BT2020);
static_assert(kTransferSmpte170m == ADATASPACE_TRANSFER_SMPTE_170M);
static_assert(kTransferSt2084 == ADATASPACE_TRANSFER_ST2084);
static_assert(kTransferHlg == ADATASPACE_TRANSFER_HLG);
static_assert(kRangeFull == ADATASPACE_RANGE_FULL);
static_assert(kRangeLimited == ADATASPACE_RANGE_LIMITED);
// The field combinations dataspace_for() returns are the NDK's named dataspaces.
static_assert((kStandardBt2020 | kTransferSt2084 | kRangeFull) == ADATASPACE_BT2020_PQ);
static_assert((kStandardBt2020 | kTransferHlg | kRangeFull) == ADATASPACE_BT2020_HLG);
static_assert((kStandardBt709 | kTransferSmpte170m | kRangeLimited) == ADATASPACE_BT709);

namespace {

constexpr const char* kLogTag = "OVT";
// Normative tag of the `dataspace=<name>` line (chapter 13 §13.4.4; chapter 14 §14.11 and the phase-0 demo grep it).
constexpr const char* kHdrLogTag = "OVT_HDR";

struct State {
  std::mutex mutex;
  std::vector<std::int32_t> hdr_types;
  float headroom = 1.0F;
  bool hdr_disabled = false;  // set by RND-0050 for the rest of the process (§10.4.3 "for this session")

  ANativeWindow* window = nullptr;
  OutputEncoding encoding = OutputEncoding::Srgb8;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
};

State& state() {
  static State instance;
  return instance;
}

bool has_extension(EGLDisplay display, const char* name) {
  const char* list = eglQueryString(display, EGL_EXTENSIONS);
  if (list == nullptr) {
    return false;
  }
  const std::string padded = std::string(" ") + list + " ";
  return padded.find(std::string(" ") + name + " ") != std::string::npos;
}

// A window-surface ES3 config with exactly `bits` per colour channel (10 + alpha 2 for HDR, 8 + alpha 8 for SDR).
// eglChooseConfig sorts larger depths first, so an exact match is picked by hand (no FP16 or 8-bit stand-in).
EGLConfig choose_config(EGLDisplay display, int bits) {
  const int alpha = bits == 10 ? 2 : 8;
  const std::array<EGLint, 13> attribs{EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_ES3_BIT,
                                       EGL_SURFACE_TYPE,
                                       EGL_WINDOW_BIT,
                                       EGL_RED_SIZE,
                                       bits,
                                       EGL_GREEN_SIZE,
                                       bits,
                                       EGL_BLUE_SIZE,
                                       bits,
                                       EGL_ALPHA_SIZE,
                                       alpha,
                                       EGL_NONE};
  EGLint count = 0;
  if (eglChooseConfig(display, attribs.data(), nullptr, 0, &count) != EGL_TRUE || count <= 0) {
    return nullptr;
  }
  std::vector<EGLConfig> configs(static_cast<std::size_t>(count));
  if (eglChooseConfig(display, attribs.data(), configs.data(), count, &count) != EGL_TRUE) {
    return nullptr;
  }
  for (EGLint i = 0; i < count; ++i) {
    EGLConfig config = configs[static_cast<std::size_t>(i)];
    EGLint red = 0;
    EGLint green = 0;
    EGLint blue = 0;
    EGLint alpha_size = 0;
    eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &alpha_size);
    if (red == bits && green == bits && blue == bits && alpha_size == alpha) {
      return config;
    }
  }
  return nullptr;
}

// Sets the dataspace of `encoding` on the window. Returns ANativeWindow_setBuffersDataSpace's result (0 = success).
int set_dataspace(ANativeWindow* window, OutputEncoding encoding) {
  return ANativeWindow_setBuffersDataSpace(window, dataspace_for(encoding));
}

void report_rnd_0050(State& s, int rc) {
  if (!s.hdr_disabled) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "RND-0050 ANativeWindow_setBuffersDataSpace failed (%d); HDR output disabled for this session",
                        rc);
  }
  s.hdr_disabled = true;
}

void destroy_egl(State& s) {
  if (s.display != EGL_NO_DISPLAY) {
    eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s.surface != EGL_NO_SURFACE) {
      eglDestroySurface(s.display, s.surface);
    }
    if (s.context != EGL_NO_CONTEXT) {
      eglDestroyContext(s.display, s.context);
    }
    // No eglTerminate: Qt's GLES context lives on the same EGLDisplay in this process.
  }
  s.surface = EGL_NO_SURFACE;
  s.context = EGL_NO_CONTEXT;
  s.display = EGL_NO_DISPLAY;
}

// Creates context and window surface for `encoding` on s.window. Returns false (and cleans up) on failure.
bool create_egl(State& s, OutputEncoding encoding) {
  s.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s.display == EGL_NO_DISPLAY || eglInitialize(s.display, nullptr, nullptr) != EGL_TRUE) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "hdr_surface: eglInitialize failed (0x%x)", eglGetError());
    s.display = EGL_NO_DISPLAY;
    return false;
  }
  const bool hdr = encoding != OutputEncoding::Srgb8;
  EGLConfig config = choose_config(s.display, hdr ? 10 : 8);
  if (config == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag, "hdr_surface: no %s EGL config", hdr ? "RGB10A2" : "RGBA8");
    destroy_egl(s);
    return false;
  }
  const std::array<EGLint, 3> context_attribs{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  s.context = eglCreateContext(s.display, config, EGL_NO_CONTEXT, context_attribs.data());
  if (s.context == EGL_NO_CONTEXT) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "hdr_surface: eglCreateContext failed (0x%x)", eglGetError());
    destroy_egl(s);
    return false;
  }
  // Tag the EGL surface too when the driver knows the BT.2020 colourspaces (§10.4.3 step 3). The values are written
  // as they are: the pattern is already PQ/HLG encoded.
  const char* extension =
      encoding == OutputEncoding::Pq10 ? "EGL_EXT_gl_colorspace_bt2020_pq" : "EGL_EXT_gl_colorspace_bt2020_hlg";
  if (hdr && has_extension(s.display, extension)) {
    const std::array<EGLint, 3> surface_attribs{
        EGL_GL_COLORSPACE_KHR,
        encoding == OutputEncoding::Pq10 ? EGL_GL_COLORSPACE_BT2020_PQ_EXT : EGL_GL_COLORSPACE_BT2020_HLG_EXT,
        EGL_NONE};
    s.surface = eglCreateWindowSurface(s.display, config, s.window, surface_attribs.data());
  }
  if (s.surface == EGL_NO_SURFACE) {
    s.surface = eglCreateWindowSurface(s.display, config, s.window, nullptr);
  }
  if (s.surface == EGL_NO_SURFACE) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "hdr_surface: eglCreateWindowSurface failed (0x%x)", eglGetError());
    destroy_egl(s);
    return false;
  }
  return true;
}

bool draw_pattern(State& s) {
  if (eglMakeCurrent(s.display, s.surface, s.surface, s.context) != EGL_TRUE) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "hdr_surface: eglMakeCurrent failed (0x%x)", eglGetError());
    return false;
  }
  EGLint width = 0;
  EGLint height = 0;
  eglQuerySurface(s.display, s.surface, EGL_WIDTH, &width);
  eglQuerySurface(s.display, s.surface, EGL_HEIGHT, &height);
  glViewport(0, 0, width, height);
  glDisable(GL_DITHER);  // the codes are exact; GLES dithering would perturb them
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  constexpr float kMaxCode = 1023.0F;
  for (const PatternRect& rect : build_test_pattern(width, height, s.encoding)) {
    // GL's scissor origin is the bottom-left corner; the pattern's is the top-left one.
    glScissor(rect.x, height - rect.y - rect.height, rect.width, rect.height);
    glClearColor(static_cast<float>(rect.code[0]) / kMaxCode, static_cast<float>(rect.code[1]) / kMaxCode,
                 static_cast<float>(rect.code[2]) / kMaxCode, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glDisable(GL_SCISSOR_TEST);
  const bool swapped = eglSwapBuffers(s.display, s.surface) == EGL_TRUE;
  if (!swapped) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "hdr_surface: eglSwapBuffers failed (0x%x)", eglGetError());
  }
  eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return swapped;
}

void detach_locked(State& s) {
  destroy_egl(s);
  if (s.window != nullptr) {
    ANativeWindow_release(s.window);
  }
  s.window = nullptr;
  s.encoding = OutputEncoding::Srgb8;
}

// SPEC-ASSUMPTION(#436): tags s.window with `encoding` and logs the normative line. The tag is applied before the EGL
// surface exists (§10.4.3 step 1) and again after it, because libEGL's eglCreateWindowSurface sets the window dataspace
// from EGL_GL_COLORSPACE (UNKNOWN when none is given) and SurfaceFlinger takes the dataspace of each queued buffer.
// Returns false when an HDR tag was refused (RND-0050) and the caller must fall back to Srgb8.
bool tag_and_create(State& s, OutputEncoding encoding) {
  int rc = set_dataspace(s.window, encoding);
  if (rc == 0 && create_egl(s, encoding)) {
    rc = set_dataspace(s.window, encoding);
    if (rc != 0) {
      destroy_egl(s);
    }
  } else if (rc == 0) {
    return false;  // EGL failed; logged by create_egl
  }
  if (rc != 0) {
    if (encoding != OutputEncoding::Srgb8) {
      report_rnd_0050(s, rc);
    } else {
      __android_log_print(ANDROID_LOG_WARN, kLogTag, "hdr_surface: SDR dataspace refused (%d)", rc);
    }
    return false;
  }
  s.encoding = encoding;
  __android_log_print(ANDROID_LOG_INFO, kHdrLogTag, "dataspace=%s", dataspace_log_name(encoding));
  return true;
}

}  // namespace

void hdr_set_display(std::span<const std::int32_t> hdr_types, const DisplayInfo& info) noexcept {
  State& s = state();
  const std::lock_guard lock(s.mutex);
  try {
    s.hdr_types.assign(hdr_types.begin(), hdr_types.end());
    s.headroom = info.hdr_sdr_ratio;
    std::string types;
    for (const std::int32_t type : s.hdr_types) {
      types += (types.empty() ? "" : ",") + std::to_string(type);
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "display: api=%d hdr_types=[%s] max_nits=%.1f min_nits=%.4f max_fall_nits=%.1f "
                        "hdr_sdr_ratio=%.3f encoding=%s%s",
                        info.api_level, types.c_str(), static_cast<double>(info.max_nits),
                        static_cast<double>(info.min_nits), static_cast<double>(info.max_fall_nits),
                        static_cast<double>(info.hdr_sdr_ratio), dataspace_log_name(choose_encoding(s.hdr_types)),
                        s.hdr_disabled ? " (HDR disabled: RND-0050)" : "");
  } catch (const std::exception&) {
    s.hdr_types.clear();  // out of memory: SDR is always safe
  }
}

bool hdr_surface_attach(ANativeWindow* window) noexcept {
  if (window == nullptr) {
    return false;
  }
  State& s = state();
  const std::lock_guard lock(s.mutex);
  if (window == s.window) {
    ANativeWindow_release(window);  // the extra reference of ANativeWindow_fromSurface
    return s.surface != EGL_NO_SURFACE && draw_pattern(s);
  }
  detach_locked(s);
  s.window = window;
  const OutputEncoding wanted = s.hdr_disabled ? OutputEncoding::Srgb8 : choose_encoding(s.hdr_types);
  bool ready = tag_and_create(s, wanted);
  if (!ready && wanted != OutputEncoding::Srgb8) {
    ready = tag_and_create(s, OutputEncoding::Srgb8);  // RND-0050 or no 10-bit config: SDR fallback
  }
  return ready && draw_pattern(s);
}

void hdr_surface_detach() noexcept {
  State& s = state();
  const std::lock_guard lock(s.mutex);
  detach_locked(s);
}

void hdr_set_headroom(float hdr_sdr_ratio) noexcept {
  State& s = state();
  const std::lock_guard lock(s.mutex);
  s.headroom = hdr_sdr_ratio;
  __android_log_print(ANDROID_LOG_DEBUG, kLogTag, "display: hdr_sdr_ratio=%.3f", static_cast<double>(hdr_sdr_ratio));
}

OutputEncoding hdr_active_encoding() noexcept {
  State& s = state();
  const std::lock_guard lock(s.mutex);
  return s.surface != EGL_NO_SURFACE ? s.encoding : OutputEncoding::Srgb8;
}

}  // namespace ovt::android_shell
