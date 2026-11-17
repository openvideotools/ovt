// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-A.403's on-device self-check; see picture_selftest.h. Compiled only into Android builds with PictureHost, and
// guarded as well, because the macOS clang-tidy job parses every changed file under app/ without Android headers.

#if defined(__ANDROID__)

#include "picture_selftest.h"

#include <rhi/qrhi.h>
#include <sys/system_properties.h>
#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJniObject>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSize>
#include <QTimer>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "ovt/render/video_surface.h"
#include "picture_frame.h"
#include "picture_host.h"
#include "surface/output_download.h"  // ovt::render::detail::download_output (exported)

namespace ovt::app {

namespace {

constexpr int kAt2Frames = 300;
constexpr int kAt2Width = 320;
constexpr int kAt2Height = 180;
constexpr int kStartDelayMs = 3000;  // after QtQuickView's window is up

bool enabled() {
  std::array<char, PROP_VALUE_MAX> value{};
  if (__system_property_get("debug.ovt.picture_selftest", value.data()) <= 0 || std::string(value.data()) != "1") {
    return false;
  }
  // Debuggable APKs only: the release APKs never run it, whatever the property says.
  const QJniObject context = QNativeInterface::QAndroidApplication::context();
  if (!context.isValid()) {
    return false;
  }
  const QJniObject info = context.callObjectMethod("getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
  constexpr jint kFlagDebuggable = 0x2;  // ApplicationInfo.FLAG_DEBUGGABLE
  return info.isValid() && (info.getField<jint>("flags") & kFlagDebuggable) != 0;
}

ovt_ColorDescription bt709_limited_10bit_420() {
  ovt_ColorDescription color{};
  color.bit_depth = 10;
  color.chroma_format = 1;
  color.primaries = 1;
  color.transfer = 1;
  color.matrix = 1;
  color.full_range = 0;
  color.chroma_siting = 1;
  return color;
}

// A 10-bit 4:2:0 limited BT.709 picture with luma luma(x, y) and Cb = Cr = 512.
template <typename Luma>
std::shared_ptr<PictureFrame> make_frame(int w, int h, Luma luma) {
  auto frame =
      make_picture_frame(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), bt709_limited_10bit_420());
  for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(h); ++y) {
    for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(w); ++x) {
      frame->set(0, x, y, luma(x, y));
    }
  }
  for (std::size_t plane = 1; plane < 3; ++plane) {
    const ovt::render::PlaneData& p = frame->picture.planes[plane];
    for (std::uint32_t y = 0; y < p.height; ++y) {
      for (std::uint32_t x = 0; x < p.width; ++x) {
        frame->set(plane, x, y, 512);
      }
    }
  }
  return frame;
}

// Frame 0 of sdr709_ramp.mp4 (tests/corpora/patterns/sdr709_ramp.expected.json, which the APK does not carry):
// 1920x1080, code(x) = round(876 * x / 1919 + 64) on every row.
std::shared_ptr<PictureFrame> sdr709_ramp_frame0() {
  return make_frame(1920, 1080, [](std::uint32_t x, std::uint32_t /*y*/) {
    return static_cast<std::uint32_t>(std::lround((876.0 * x / 1919.0) + 64.0));
  });
}

// Picture `f` of AT2: diagonal stripes that move with f, so every picture differs from the one before everywhere
// along the stripes.
std::shared_ptr<PictureFrame> at2_frame(int f) {
  return make_frame(kAt2Width, kAt2Height, [f](std::uint32_t x, std::uint32_t y) {
    return 64U + (((x * 3U) + (y * 5U) + (static_cast<std::uint32_t>(f) * 29U)) % 877U);
  });
}

// Pixels whose R, G or B differs by more than 1 (as WI-1.405's desktop test; alpha is the scene graph's).
std::size_t differing_pixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  if (a.size() != b.size()) {
    return a.size() / 4;
  }
  std::size_t n = 0;
  for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
    for (std::size_t c = 0; c < 3; ++c) {
      if (std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])) > 1) {
        ++n;
        break;
      }
    }
  }
  return n;
}

// The libplacebo download of a picture rendered 1:1 by an Interop::None surface (the surface's own Vulkan device).
class Reference {
 public:
  bool init(int w, int h, std::string& why) {
    surface_ = ovt::render::VideoSurface::create("selftest-reference");
    ovt::render::DisplayTarget target;
    target.width = w;
    target.height = h;
    if (!surface_->init(target)) {
      why = std::string(surface_->last_error());
      return false;
    }
    w_ = w;
    h_ = h;
    return true;
  }

  std::vector<std::uint8_t> pixels(const PictureFrame& frame, bool dither) {
    surface_->set_frame(frame.picture);
    ovt::render::ViewState view;
    view.zoom = 1.0;
    view.pan_x = w_ / 2.0;
    view.pan_y = h_ / 2.0;
    view.dither = dither;
    surface_->render(view);
    ovt::render::detail::OutputPixels px;
    if (!ovt::render::detail::download_output(*surface_, px) || px.texel_bytes != 4) {
      return {};
    }
    return px.bytes;
  }

 private:
  std::unique_ptr<ovt::render::VideoSurface> surface_;
  int w_ = 0;
  int h_ = 0;
};

// A QQuickWindow holding one PictureHost, rendered into a texture through QQuickRenderControl on this thread (the
// app's own window renders on Qt Quick's render thread with another context, so the two do not meet).
class Harness {
 public:
  Harness() = default;
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;
  Harness(Harness&&) = delete;
  Harness& operator=(Harness&&) = delete;
  ~Harness() {
    window_.reset();  // the item and its renderer first, before the render target
    rp_.reset();
    rt_.reset();
    tex_.reset();
  }

  bool init(QSize size, std::string& why) {
    window_ = std::make_unique<QQuickWindow>(&control_);
    window_->setColor(QColor(0x00, 0xFF, 0x00));
    if (!control_.initialize()) {
      why = "QQuickRenderControl::initialize failed";
      return false;
    }
    rhi_ = control_.rhi();
    tex_.reset(
        rhi_->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    rt_.reset(tex_->create() ? rhi_->newTextureRenderTarget({QRhiColorAttachment(tex_.get())}) : nullptr);
    if (rt_ == nullptr) {
      why = "could not create the render target texture";
      return false;
    }
    rp_.reset(rt_->newCompatibleRenderPassDescriptor());
    rt_->setRenderPassDescriptor(rp_.get());
    if (!rt_->create()) {
      why = "could not create the render target";
      return false;
    }
    window_->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(rt_.get()));
    window_->setGeometry(0, 0, size.width(), size.height());
    window_->contentItem()->setSize(size);
    host_ = new PictureHost(window_->contentItem());  // NOLINT(cppcoreguidelines-owning-memory): the item owns it
    host_->setSize(size);
    size_ = size;
    return true;
  }

  // Renders one frame and returns its RGBA8 pixels, top row first (empty on a failed readback).
  std::vector<std::uint8_t> frame() {
    QCoreApplication::processEvents();
    control_.polishItems();
    control_.beginFrame();
    control_.sync();
    control_.render();
    QRhiReadbackResult result;
    QRhiResourceUpdateBatch* rub = rhi_->nextResourceUpdateBatch();
    rub->readBackTexture(QRhiReadbackDescription(tex_.get()), &result);
    control_.commandBuffer()->resourceUpdate(rub);
    control_.endFrame();  // an offscreen frame waits for the GPU, so the readback is complete
    QCoreApplication::processEvents();
    if (result.pixelSize != size_) {
      return {};
    }
    return {result.data.cbegin(), result.data.cend()};
  }

  [[nodiscard]] PictureHost& host() const { return *host_; }
  [[nodiscard]] QString backend() const {
    return rhi_ != nullptr ? QString::fromLatin1(rhi_->backendName()) : QString();
  }

 private:
  QQuickRenderControl control_;
  std::unique_ptr<QQuickWindow> window_;
  QRhi* rhi_ = nullptr;
  PictureHost* host_ = nullptr;
  std::unique_ptr<QRhiTexture> tex_;
  std::unique_ptr<QRhiTextureRenderTarget> rt_;
  std::unique_ptr<QRhiRenderPassDescriptor> rp_;
  QSize size_;
};

void fail(int at, const std::string& why) {
  qInfo("OVT: picture-selftest AT%d result=FAIL reason=\"%s\"", at, why.c_str());
}

// AT1: the Qt Quick window showing frame 0 of sdr709_ramp equals the Interop::None render within 0.1 %.
void run_at1() {
  const std::shared_ptr<PictureFrame> frame = sdr709_ramp_frame0();
  const int w = static_cast<int>(frame->picture.coded_width);
  const int h = static_cast<int>(frame->picture.coded_height);
  Harness harness;
  std::string why;
  if (!harness.init(QSize(w, h), why)) {
    fail(1, why);
    return;
  }
  harness.host().set_picture(frame);
  static_cast<void>(harness.frame());
  harness.host().update();  // the renderer's stats reach the item in the next synchronize()
  const std::vector<std::uint8_t> shown = harness.frame();
  const QString interop = harness.host().interop();
  Reference reference;
  if (!reference.init(w, h, why)) {
    fail(1, "reference surface: " + why);
    return;
  }
  const std::vector<std::uint8_t> expected = reference.pixels(*frame, true);
  const std::size_t total = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  const std::size_t differ = differing_pixels(shown, expected);
  const bool pass = !shown.empty() && !expected.empty() && interop == QStringLiteral("gl-texture") &&
                    harness.host().last_error().isEmpty() && differ * 1000 <= total;
  qInfo("OVT: picture-selftest AT1 backend=%s interop=%s size=%dx%d differ=%zu/%zu error=\"%s\" result=%s",
        qPrintable(harness.backend()), qPrintable(interop), w, h, differ, total,
        qPrintable(harness.host().last_error()), pass ? "PASS" : "FAIL");
}

// AT2: 300 pictures through the fence path; every presented frame equals its Interop::None render (dither off in
// both, so the two renders are deterministic).
void run_at2() {
  Harness harness;
  std::string why;
  if (!harness.init(QSize(kAt2Width, kAt2Height), why)) {
    fail(2, why);
    return;
  }
  Reference reference;
  if (!reference.init(kAt2Width, kAt2Height, why)) {
    fail(2, "reference surface: " + why);
    return;
  }
  harness.host().set_dither(false);
  int bad = 0;
  int exact = 0;
  int first_bad = -1;
  std::size_t worst = 0;
  for (int f = 0; f < kAt2Frames; ++f) {
    const std::shared_ptr<PictureFrame> frame = at2_frame(f);
    harness.host().set_picture(frame);
    const std::vector<std::uint8_t> shown = harness.frame();
    const std::vector<std::uint8_t> expected = reference.pixels(*frame, false);
    const std::size_t differ = expected.empty() ? shown.size() / 4 : differing_pixels(shown, expected);
    if (shown.empty() || differ != 0) {
      ++bad;
      first_bad = first_bad < 0 ? f : first_bad;
      worst = differ > worst ? differ : worst;
    } else if (shown == expected) {
      ++exact;
    }
  }
  const QString interop = harness.host().interop();
  const bool pass = bad == 0 && interop == QStringLiteral("gl-texture");
  qInfo(
      "OVT: picture-selftest AT2 interop=%s frames=%d rendered=%d bad=%d first_bad=%d worst_differ=%zu exact=%d "
      "gl_finish=%s result=%s",
      qPrintable(interop), kAt2Frames, harness.host().frames_rendered(), bad, first_bad, worst, exact,
      qEnvironmentVariableIsSet("OVT_RENDER_GL_FINISH") ? "1" : "0", pass ? "PASS" : "FAIL");
}

}  // namespace

void maybe_schedule_picture_selftest() {
  if (!enabled()) {
    return;
  }
  qInfo("OVT: picture-selftest scheduled in %d ms", kStartDelayMs);
  QTimer::singleShot(kStartDelayMs, qApp, [] {
    run_at1();
    run_at2();
    qInfo("OVT: picture-selftest done");
  });
}

}  // namespace ovt::app

#endif  // defined(__ANDROID__)
