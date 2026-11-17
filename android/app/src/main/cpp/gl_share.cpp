// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared GL context of EMBEDDED mode on Android (WI-A.403); see gl_share.h.

#include "gl_share.h"

#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace ovt::android_shell {

namespace {

std::string egl_error(const char* what) {
  std::array<char, 16> code{};
  const int n = std::snprintf(code.data(), code.size(), "0x%04x", static_cast<unsigned>(eglGetError()));
  return std::string(what) + " failed (EGL error " + (n > 0 ? code.data() : "?") + ")";
}

bool has_extension(EGLDisplay display, std::string_view name) {
  const char* list = eglQueryString(display, EGL_EXTENSIONS);
  if (list == nullptr) {
    return false;
  }
  // A space-separated list (EGL 1.5 §3.3).
  std::string_view rest(list);
  while (!rest.empty()) {
    const std::size_t end = rest.find(' ');
    if (rest.substr(0, end) == name) {
      return true;
    }
    rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
  }
  return false;
}

}  // namespace

bool GlShareContext::create(EGLConfig config, std::string& reason) {
  destroy();
  EGLDisplay display = eglGetCurrentDisplay();
  EGLContext parent = eglGetCurrentContext();
  if (display == EGL_NO_DISPLAY || parent == EGL_NO_CONTEXT) {
    reason = "no EGL context is current on the render thread (the shared context must be made from Qt's)";
    return false;
  }
  if (config == nullptr) {
    // The config Qt's context was made with; config id 0 means a context made with EGL_KHR_no_config_context, which
    // the shared one copies (EGL_NO_CONFIG_KHR is a null EGLConfig).
    EGLint id = 0;
    if (eglQueryContext(display, parent, EGL_CONFIG_ID, &id) == EGL_FALSE) {
      reason = egl_error("eglQueryContext(EGL_CONFIG_ID)");
      return false;
    }
    if (id != 0) {
      const std::array<EGLint, 3> attribs = {EGL_CONFIG_ID, id, EGL_NONE};
      EGLint count = 0;
      if (eglChooseConfig(display, attribs.data(), &config, 1, &count) == EGL_FALSE || count != 1) {
        reason = egl_error("eglChooseConfig(EGL_CONFIG_ID)");
        return false;
      }
    }
  }
  if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
    reason = egl_error("eglBindAPI(EGL_OPENGL_ES_API)");
    return false;
  }
  // GLES 3.0: libplacebo's GL backend and the fence of the §10.2.2 Android row (glFenceSync) need it.
  const std::array<EGLint, 3> context_attribs = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, parent, context_attribs.data());
  if (context == EGL_NO_CONTEXT) {
    reason = egl_error("eglCreateContext(share = Qt's context)");
    return false;
  }
  display_ = display;
  parent_ = parent;
  context_ = context;
  if (!has_extension(display, "EGL_KHR_surfaceless_context")) {
    if (config == nullptr) {
      reason = "Qt's context has no EGLConfig and EGL_KHR_surfaceless_context is missing";
      destroy();
      return false;
    }
    const std::array<EGLint, 5> pbuffer_attribs = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display, config, pbuffer_attribs.data());
    if (surface_ == EGL_NO_SURFACE) {
      reason = egl_error("eglCreatePbufferSurface(1x1)");
      destroy();
      return false;
    }
  }
  // Check the context can be made current with its drawable (and leave Qt's current, as found).
  const Scope probe(this);
  if (!probe.active()) {
    reason = egl_error("eglMakeCurrent(shared context)");
    destroy();
    return false;
  }
  return true;
}

void GlShareContext::destroy() {
  if (display_ == EGL_NO_DISPLAY) {
    return;
  }
  if (eglGetCurrentContext() == context_) {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (surface_ != EGL_NO_SURFACE) {
    eglDestroySurface(display_, surface_);
  }
  if (context_ != EGL_NO_CONTEXT) {
    eglDestroyContext(display_, context_);
  }
  display_ = EGL_NO_DISPLAY;
  parent_ = EGL_NO_CONTEXT;
  context_ = EGL_NO_CONTEXT;
  surface_ = EGL_NO_SURFACE;
}

GlShareContext::Scope::Scope(const GlShareContext* share) {
  if (share == nullptr || !share->valid()) {
    return;
  }
  display_ = eglGetCurrentDisplay();
  draw_ = eglGetCurrentSurface(EGL_DRAW);
  read_ = eglGetCurrentSurface(EGL_READ);
  context_ = eglGetCurrentContext();
  active_ = eglMakeCurrent(share->display_, share->surface_, share->surface_, share->context_) == EGL_TRUE;
}

GlShareContext::Scope::~Scope() {
  if (!active_) {
    return;
  }
  if (context_ != EGL_NO_CONTEXT) {
    eglMakeCurrent(display_, draw_, read_, context_);
  } else {
    eglMakeCurrent(eglGetCurrentDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
}

void GlShareContext::wait_on_current(void* gl_sync) {
  if (gl_sync == nullptr) {
    return;
  }
  // Fence objects are shared across the contexts of one share group (GLES 3.0 §5.3), so Qt's context can wait on the
  // fence the surface's context inserted.
  glWaitSync(static_cast<GLsync>(gl_sync), 0, GL_TIMEOUT_IGNORED);
}

}  // namespace ovt::android_shell
