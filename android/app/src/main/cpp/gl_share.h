// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GL context libovt-render draws into in EMBEDDED mode on Android (chapter 10 §10.2.2 Android row, WI-A.403):
// an EGL context in the share group of Qt's, created on Qt Quick's render thread from Qt's context while that is
// current there (§10.11 WI-A.403 pitfall: a context created elsewhere fails with EGL_BAD_MATCH). The surface's output
// texture name and its fence (a GLES 3.0 GLsync) are therefore valid in Qt's context too:
//
//   1. GlShareContext::create() once, with Qt's context current.
//   2. Every VideoSurface call inside a GlShareContext::Scope, which makes the shared context current and restores
//      whatever was current before (Qt's context) when it ends.
//   3. On Qt's context, wait_on_current() on the ExternalTexture's GLsync before sampling the texture.
//
// SPEC-ASSUMPTION(#725): libovt-render's pl_opengl_create() takes the current context (no make/release-current
// callbacks: DisplayTarget has no field for them), so the Scope brackets every call instead; and there is no
// EGL_KHR_fence_sync branch, because a context outside Qt's share group could not see the texture name at all.
//
// Plain EGL + GLES 3.0 and no Qt, so that the host test (android/app/src/test/cpp) runs the same code on Mesa.

#ifndef OVT_ANDROID_GL_SHARE_H
#define OVT_ANDROID_GL_SHARE_H

#include <EGL/egl.h>

#include <string>

namespace ovt::android_shell {

class GlShareContext {
 public:
  GlShareContext() = default;
  GlShareContext(const GlShareContext&) = delete;
  GlShareContext& operator=(const GlShareContext&) = delete;
  GlShareContext(GlShareContext&&) = delete;
  GlShareContext& operator=(GlShareContext&&) = delete;
  ~GlShareContext() { destroy(); }

  // Creates a GLES 3 context sharing with the context current on this thread, with the config that context was
  // created with (`config`; EGL_NO_CONFIG_KHR asks EGL for it). Its drawable is EGL_NO_SURFACE where
  // EGL_KHR_surfaceless_context exists, else a 1x1 pbuffer of the same config. On failure returns false with the
  // reason (and the EGL error) in `reason`.
  bool create(EGLConfig config, std::string& reason);
  void destroy();

  [[nodiscard]] bool valid() const noexcept { return context_ != EGL_NO_CONTEXT; }
  [[nodiscard]] EGLContext context() const noexcept { return context_; }
  [[nodiscard]] EGLContext share_parent() const noexcept { return parent_; }

  // Makes the shared context current for its lifetime and restores the previous display, surfaces and context.
  // A Scope of an invalid GlShareContext (or of nullptr) does nothing.
  class Scope {
   public:
    explicit Scope(const GlShareContext* share);
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
    ~Scope();
    [[nodiscard]] bool active() const noexcept { return active_; }

   private:
    bool active_ = false;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface draw_ = EGL_NO_SURFACE;
    EGLSurface read_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
  };

  // §10.2.2 fence rule, consumer half: makes the context current on this thread (Qt's, one of the share group) wait
  // on the GPU for `gl_sync`, the GLsync the surface inserted after its last render (ExternalTexture::handle2).
  // nullptr (OVT_RENDER_GL_FINISH=1: the surface already called glFinish) waits for nothing. glWaitSync does not
  // block the CPU; the wait is queued before the commands Qt issues next on this context.
  static void wait_on_current(void* gl_sync);

 private:
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext parent_ = EGL_NO_CONTEXT;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;  // the 1x1 pbuffer without EGL_KHR_surfaceless_context
};

}  // namespace ovt::android_shell

#endif  // OVT_ANDROID_GL_SHARE_H
