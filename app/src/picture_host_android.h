// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Android row of PictureHost's interop (chapter 10 §10.2.2 Android (EMBEDDED) row, WI-A.403). Qt Quick renders
// with OpenGL ES on Android; PictureHost's renderer then hands libovt-render a GL context shared with Qt's
// (Interop::GlTexture) and wraps the surface's output texture name with QRhiTexture::createFrom({GLuint, 0}):
//
//   - AndroidGlInterop::create() on the render thread, with Qt's context current, before VideoSurface::init();
//   - every VideoSurface call inside a SurfaceGlScope (the shared context current, Qt's restored after);
//   - AndroidGlInterop::wait() on Qt's context before Qt samples the texture (the §10.2.2 fence rule).
//
// On every other platform the types below are empty, so picture_host.cpp needs no #ifdef around the scopes.

#ifndef OVT_APP_PICTURE_HOST_ANDROID_H
#define OVT_APP_PICTURE_HOST_ANDROID_H

#include <QString>

#include <memory>

class QRhi;

#if defined(__ANDROID__)
#include "gl_share.h"
#endif

namespace ovt::app {

#if defined(__ANDROID__)

class AndroidGlInterop {
 public:
  // The shared context for `rhi` (render thread, GLES backend only); nullptr with `reason` when the backend is not
  // OpenGL ES or the context cannot be made (the renderer then copies through the CPU).
  static std::unique_ptr<AndroidGlInterop> create(QRhi* rhi, QString& reason);

  [[nodiscard]] const android_shell::GlShareContext* share() const noexcept { return &share_; }

  // Queues, on Qt's context, the GPU wait for the surface's last render (ExternalTexture::handle2; nullptr with
  // OVT_RENDER_GL_FINISH=1). Call after the SurfaceGlScope of the render has ended.
  void wait(QRhi* rhi, void* gl_sync) const;

 private:
  android_shell::GlShareContext share_;
};

// The shared context current around VideoSurface calls; nothing without an interop.
class SurfaceGlScope {
 public:
  explicit SurfaceGlScope(const std::unique_ptr<AndroidGlInterop>& gl)
      : scope_(gl != nullptr ? gl->share() : nullptr) {}

 private:
  android_shell::GlShareContext::Scope scope_;
};

#else

class AndroidGlInterop {  // never created off Android
 public:
  void wait(QRhi* /*rhi*/, void* /*gl_sync*/) const {}
};

class SurfaceGlScope {
 public:
  explicit SurfaceGlScope(const std::unique_ptr<AndroidGlInterop>& /*gl*/) {}
};

#endif

}  // namespace ovt::app

#endif  // OVT_APP_PICTURE_HOST_ANDROID_H
