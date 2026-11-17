// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-A.403: the shared EGL context of EMBEDDED mode (android/app/src/main/cpp/gl_share.cpp) on the host's EGL (Mesa
// in CI). A plain GLES 3 context stands in for Qt's: GlShareContext is made from it while it is current, the
// "surface" side draws into a texture on the shared context and fences it, and the "Qt" side waits on the fence and
// reads the texture through its own framebuffer. Skipped where the host has no EGL display or GLES 3 context.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "gl_share.h"

namespace {

using ovt::android_shell::GlShareContext;

constexpr int kSize = 64;
constexpr int kStripes = 16;  // scissored clears per frame: a half-finished frame shows mixed stripes

// A GLES 3 context current on this thread, standing in for Qt Quick's on the render thread.
class HostContext {
 public:
  HostContext() {
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display != nullptr) {
      display_ = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (display_ == EGL_NO_DISPLAY) {
      display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (display_ == EGL_NO_DISPLAY || eglInitialize(display_, nullptr, nullptr) == EGL_FALSE) {
      why_ = "no EGL display";
      display_ = EGL_NO_DISPLAY;
      return;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const std::array<EGLint, 5> config_attribs = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
                                                  EGL_PBUFFER_BIT, EGL_NONE};
    EGLint count = 0;
    if (eglChooseConfig(display_, config_attribs.data(), &config_, 1, &count) == EGL_FALSE || count != 1) {
      why_ = "no GLES 3 pbuffer EGLConfig";
      return;
    }
    const std::array<EGLint, 3> context_attribs = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs.data());
    const std::array<EGLint, 5> pbuffer_attribs = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, config_, pbuffer_attribs.data());
    if (context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE ||
        eglMakeCurrent(display_, surface_, surface_, context_) == EGL_FALSE) {
      why_ = "cannot make a GLES 3 context current";
      return;
    }
    ok_ = true;
  }
  HostContext(const HostContext&) = delete;
  HostContext& operator=(const HostContext&) = delete;
  HostContext(HostContext&&) = delete;
  HostContext& operator=(HostContext&&) = delete;
  ~HostContext() {
    if (display_ == EGL_NO_DISPLAY) {
      return;
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface_ != EGL_NO_SURFACE) {
      eglDestroySurface(display_, surface_);
    }
    if (context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(display_, context_);
    }
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::string& why() const noexcept { return why_; }
  [[nodiscard]] EGLContext context() const noexcept { return context_; }
  [[nodiscard]] EGLSurface surface() const noexcept { return surface_; }
  [[nodiscard]] EGLConfig config() const noexcept { return config_; }

 private:
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLConfig config_ = nullptr;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  bool ok_ = false;
  std::string why_;
};

// The colour of stripe `s` of frame `f` (RGBA8).
std::array<std::uint8_t, 4> stripe_colour(int f, int s) {
  return {static_cast<std::uint8_t>((f * 7 + s * 13) & 0xFF), static_cast<std::uint8_t>((f * 3) & 0xFF),
          static_cast<std::uint8_t>((s * 16) & 0xFF), 0xFF};
}

std::uint64_t checksum(const std::vector<std::uint8_t>& px) {
  std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
  for (const std::uint8_t b : px) {
    h = (h ^ b) * 1099511628211ULL;
  }
  return h;
}

// The bytes frame `f` must read back as: stripe s covers rows [s * kSize / kStripes, (s + 1) * kSize / kStripes).
std::vector<std::uint8_t> expected_frame(int f) {
  std::vector<std::uint8_t> px(static_cast<std::size_t>(kSize) * kSize * 4);
  for (int y = 0; y < kSize; ++y) {
    const auto c = stripe_colour(f, (y * kStripes) / kSize);
    for (int x = 0; x < kSize; ++x) {
      const std::size_t at = ((static_cast<std::size_t>(y) * kSize) + static_cast<std::size_t>(x)) * 4;
      for (std::size_t i = 0; i < 4; ++i) {
        px[at + i] = c[i];
      }
    }
  }
  return px;
}

// Frame `f` on the shared context into the framebuffer `fbo`, as VideoSurface::render() would, ending like the
// surface's end_frame(): a GLES 3.0 fence, flushed so that another context can wait on it.
GLsync draw_frame_fenced(const GlShareContext& share, GLuint fbo, int f) {
  const GlShareContext::Scope scope(&share);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(0, 0, kSize, kSize);
  glEnable(GL_SCISSOR_TEST);
  for (int s = 0; s < kStripes; ++s) {
    const auto c = stripe_colour(f, s);
    glScissor(0, (s * kSize) / kStripes, kSize, kSize / kStripes);
    glClearColor(static_cast<float>(c[0]) / 255.0F, static_cast<float>(c[1]) / 255.0F,
                 static_cast<float>(c[2]) / 255.0F, static_cast<float>(c[3]) / 255.0F);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glDisable(GL_SCISSOR_TEST);
  GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();
  return fence;
}

// A framebuffer of the current context with `tex` as its colour attachment; 0 when incomplete.
GLuint attach_framebuffer(GLuint tex) {
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    glDeleteFramebuffers(1, &fbo);
    return 0;
  }
  return fbo;
}

}  // namespace

TEST_CASE("GlShareContext needs a current context to share with", "[android][gl_share]") {
  eglMakeCurrent(eglGetCurrentDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  GlShareContext share;
  std::string reason;
  CHECK_FALSE(share.create(nullptr, reason));
  CHECK_FALSE(share.valid());
  CHECK(reason.find("no EGL context is current") != std::string::npos);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): Catch2 macros count as branches
TEST_CASE("GlShareContext shares with the current context and Scope restores it", "[android][gl_share]") {
  const HostContext host;
  if (!host.ok()) {
    SKIP("no host GLES 3 context: " << host.why());
  }
  // Both ways of naming the config: Qt's QEGLContext::config(), and none (looked up from the current context).
  for (EGLConfig config : {host.config(), static_cast<EGLConfig>(nullptr)}) {
    GlShareContext share;
    std::string reason;
    REQUIRE(share.create(config, reason));
    CHECK(reason.empty());
    CHECK(share.share_parent() == host.context());
    CHECK(eglGetCurrentContext() == host.context());  // create() leaves the host's context current
    {
      const GlShareContext::Scope scope(&share);
      CHECK(scope.active());
      CHECK(eglGetCurrentContext() == share.context());
    }
    CHECK(eglGetCurrentContext() == host.context());
    CHECK(eglGetCurrentSurface(EGL_DRAW) == host.surface());
  }
  // A Scope without a context does nothing.
  const GlShareContext::Scope none(nullptr);
  CHECK_FALSE(none.active());
  CHECK(eglGetCurrentContext() == host.context());
  GlShareContext::wait_on_current(nullptr);  // OVT_RENDER_GL_FINISH=1: no fence, no wait
  CHECK(glGetError() == GL_NO_ERROR);
}

// WI-A.403 acceptance test 2, host half: 300 frames drawn on the shared context, each fenced and waited on by the
// host's context, which reads back exactly the frame the shared context finished (no partially drawn texture).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): Catch2 macros count as branches
TEST_CASE("Fenced frames from the shared context read back complete on the host context", "[android][gl_share]") {
  const HostContext host;
  if (!host.ok()) {
    SKIP("no host GLES 3 context: " << host.why());
  }
  GlShareContext share;
  std::string reason;
  REQUIRE(share.create(host.config(), reason));

  GLuint tex = 0;
  GLuint draw_fbo = 0;  // framebuffers are per context: this one is the shared context's
  {
    const GlShareContext::Scope scope(&share);
    REQUIRE(scope.active());
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSize, kSize);
    draw_fbo = attach_framebuffer(tex);
  }
  REQUIRE(draw_fbo != 0);
  // The host context sees the texture name (one share group) and reads it through a framebuffer of its own.
  const GLuint read_fbo = attach_framebuffer(tex);
  REQUIRE(read_fbo != 0);

  std::vector<std::uint8_t> px(static_cast<std::size_t>(kSize) * kSize * 4);
  int mismatches = 0;
  for (int f = 0; f < 300; ++f) {
    GLsync fence = draw_frame_fenced(share, draw_fbo, f);
    REQUIRE(fence != nullptr);
    GlShareContext::wait_on_current(fence);  // what PictureHost does on Qt's context before sampling
    glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glDeleteSync(fence);
    if (checksum(px) != checksum(expected_frame(f))) {
      ++mismatches;
    }
  }
  CHECK(mismatches == 0);
  CHECK(glGetError() == GL_NO_ERROR);

  glDeleteFramebuffers(1, &read_fbo);
  {
    const GlShareContext::Scope scope(&share);
    glDeleteFramebuffers(1, &draw_fbo);
    glDeleteTextures(1, &tex);
  }
}
