// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PictureHost's Android interop row (WI-A.403); see picture_host_android.h. Compiled only into Android builds, and
// guarded as well, because the macOS clang-tidy job parses every changed file under app/ without Android headers.

#if defined(__ANDROID__)

#include "picture_host_android.h"

#include <rhi/qrhi.h>
#include <QOpenGLContext>

#include <EGL/egl.h>

#include <memory>
#include <string>

namespace ovt::app {

std::unique_ptr<AndroidGlInterop> AndroidGlInterop::create(QRhi* rhi, QString& reason) {
  if (rhi == nullptr || rhi->backend() != QRhi::OpenGLES2) {
    reason = QStringLiteral("Interop::GlTexture needs Qt Quick's OpenGL ES backend");
    return nullptr;
  }
  const auto* handles = static_cast<const QRhiGles2NativeHandles*>(rhi->nativeHandles());
  QOpenGLContext* qt_context = handles != nullptr ? handles->context : nullptr;
  const auto* egl = qt_context != nullptr ? qt_context->nativeInterface<QNativeInterface::QEGLContext>() : nullptr;
  if (egl == nullptr) {
    reason = QStringLiteral("Qt's OpenGL ES context is not an EGL context");
    return nullptr;
  }
  // Step 1 and the WI pitfall: Qt made its context on this (the render) thread; the shared one is made here from the
  // context current on it, which must be Qt's, or eglCreateContext fails with EGL_BAD_MATCH.
  if (!rhi->makeThreadLocalNativeContextCurrent() || eglGetCurrentContext() != egl->nativeContext()) {
    reason = QStringLiteral("Qt's EGL context is not current on the render thread");
    return nullptr;
  }
  auto interop = std::make_unique<AndroidGlInterop>();
  std::string why;
  if (!interop->share_.create(egl->config(), why)) {
    reason = QString::fromStdString(why);
    return nullptr;
  }
  return interop;
}

void AndroidGlInterop::wait(QRhi* rhi, void* gl_sync) const {
  if (gl_sync == nullptr) {
    return;
  }
  // QRhi's GL backend keeps its context current during render(); make sure the wait lands on it, not on ours.
  if (eglGetCurrentContext() != share_.share_parent()) {
    rhi->makeThreadLocalNativeContextCurrent();
  }
  android_shell::GlShareContext::wait_on_current(gl_sync);
}

}  // namespace ovt::app

#endif  // defined(__ANDROID__)
