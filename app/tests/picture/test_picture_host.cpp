// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.405 acceptance tests (chapter 10 §10.11): PictureHost in a QQuickWindow rendered through QQuickRenderControl
// into a texture this test reads back, on the RHI backend OVT Studio uses (Vulkan on Linux, so the
// Interop::VulkanSharedDevice row of §10.2.2; Metal on macOS, the Interop::MetalTexture row).
//
//   1. The item showing frame 0 of sdr709_ramp.mp4 matches the libplacebo download of the same picture rendered by a
//      separate VideoSurface (what SdrPng8 writes), with at most 0.1 % of the pixels different.
//   2. Resizing the item calls VideoSurface::resize() once per new size, and no frame captured after a resize shows
//      only the background.
//
// SPEC-ASSUMPTION(#510): the decoder of the playback path (ovt_decoder_next, WI-2.171) is not in the tree yet, so
// frame 0 is built from tests/corpora/patterns/sdr709_ramp.expected.json (the pattern is encoded losslessly, so the
// decoded frame equals its definition), and export_png(SdrPng8) (§10.8) is not implemented yet, so the reference is
// download_output() of an Interop::None surface, which is the image SdrPng8 encodes.
//
// Linux needs an X display for Qt's Vulkan instance (the offscreen platform has none): CTest runs this binary under
// xvfb-run; without a display the tests skip. macOS uses the offscreen platform (Metal needs no window).

#include <catch2/catch_test_macros.hpp>

#include <rhi/qrhi.h>

#if QT_CONFIG(vulkan)
#include <QVersionNumber>
#include <QVulkanInstance>
#endif
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSize>
#include <QString>

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
#include "surface/output_download.h"

namespace {

using ovt::app::PictureFrame;
using ovt::app::PictureHost;

// Creates the application once, on the platform this test needs. nullptr (the caller skips) when Linux has no display.
// The application is never destroyed: tearing it down in the static destructors at exit, after Qt's own globals,
// crashes in ~QGuiApplication.
QGuiApplication* app_or_null() {
  static QGuiApplication* app = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    qputenv("QT_SCALE_FACTOR", "1");
#if defined(__APPLE__)
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
      qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#else
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
      if (qEnvironmentVariableIsEmpty("DISPLAY")) {
        return nullptr;
      }
      qputenv("QT_QPA_PLATFORM", "xcb");
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
#endif
    static int argc = 1;
    static std::array<char, 22> name{"ovt_tests_app_picture"};
    static std::array<char*, 2> argv{name.data(), nullptr};
    app = new QGuiApplication(argc, argv.data());  // NOLINT(cppcoreguidelines-owning-memory): lives until exit
  }
  return app;
}

// Frame 0 of sdr709_ramp.mp4 (corpus id syn_pat_sdr709_ramp), from its expected.json: 10-bit 4:2:0 limited BT.709,
// luma code(x) = round(876 * x / (width - 1) + 64) on every row, Cb = Cr = 512.
std::shared_ptr<PictureFrame> sdr709_ramp_frame0() {
  QFile f(QStringLiteral(OVT_PATTERNS_DIR "/sdr709_ramp.expected.json"));
  REQUIRE(f.open(QIODevice::ReadOnly));
  const QJsonObject exp = QJsonDocument::fromJson(f.readAll()).object();
  REQUIRE(exp.value(QStringLiteral("range")).toString() == QStringLiteral("limited"));
  REQUIRE(exp.value(QStringLiteral("ramp_rule"))
              .toString()
              .startsWith(QStringLiteral("code(x) = round(876 * x / 1919 + 64)")));
  const auto width = static_cast<std::uint32_t>(exp.value(QStringLiteral("width")).toInt());
  const auto height = static_cast<std::uint32_t>(exp.value(QStringLiteral("height")).toInt());
  ovt_ColorDescription color{};
  color.bit_depth = static_cast<std::uint8_t>(exp.value(QStringLiteral("bit_depth")).toInt());
  color.chroma_format = 1;
  color.primaries = 1;
  color.transfer = 1;
  color.matrix = 1;
  color.full_range = 0;
  color.chroma_siting = 1;
  auto frame = ovt::app::make_picture_frame(width, height, color);
  REQUIRE(frame != nullptr);
  const auto first = static_cast<std::uint32_t>(exp.value(QStringLiteral("ramp_first_code")).toInt());
  const auto last = static_cast<std::uint32_t>(exp.value(QStringLiteral("ramp_last_code")).toInt());
  for (std::uint32_t x = 0; x < width; ++x) {
    const auto code = static_cast<std::uint32_t>(
        std::lround((static_cast<double>(last - first) * x / static_cast<double>(width - 1)) + first));
    for (std::uint32_t y = 0; y < height; ++y) {
      frame->set(0, x, y, code);
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

#if QT_CONFIG(vulkan)
// A render-controlled window has no QVulkanInstance of its own (Qt's default one is for on-screen windows). Like Qt's
// default instance it asks for the newest API up to 1.3, so libplacebo gets the Vulkan 1.2 it needs (§10.2.2).
QVulkanInstance* vulkan_instance() {
  static QVulkanInstance inst;
  static const bool ok = [] {
    const QVersionNumber supported = inst.supportedApiVersion();
    inst.setApiVersion(supported >= QVersionNumber(1, 3) ? QVersionNumber(1, 3) : supported);
    return inst.create();
  }();
  return ok ? &inst : nullptr;
}
#endif

// A QQuickWindow whose content (one PictureHost filling it) renders into a texture through QQuickRenderControl.
class Harness {
 public:
  Harness() = default;
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;
  Harness(Harness&&) = delete;
  Harness& operator=(Harness&&) = delete;
  ~Harness() {
    // The item (and its renderer) go with the window, before the render target and the QRhi.
    window_.reset();
    rp_.reset();
    rt_.reset();
    tex_.reset();
  }

  // "" on success, else why not.
  std::string init(QSize size) {
    window_ = std::make_unique<QQuickWindow>(&control_);
#if QT_CONFIG(vulkan)
    if (QQuickWindow::graphicsApi() == QSGRendererInterface::Vulkan) {
      QVulkanInstance* inst = vulkan_instance();
      if (inst == nullptr) {
        return "QVulkanInstance::create failed";
      }
      window_->setVulkanInstance(inst);
    }
#endif
    window_->setColor(QColor(0x00, 0xFF, 0x00));  // where the item draws nothing
    if (!control_.initialize()) {
      return "QQuickRenderControl::initialize failed (no RHI device)";
    }
    rhi_ = control_.rhi();
    host_ = new PictureHost(window_->contentItem());  // owned by the content item
    resize(size);
    return "";
  }

  void resize(QSize size) {
    rp_.reset();
    rt_.reset();
    tex_.reset(
        rhi_->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    REQUIRE(tex_->create());
    rt_.reset(rhi_->newTextureRenderTarget({QRhiColorAttachment(tex_.get())}));
    rp_.reset(rt_->newCompatibleRenderPassDescriptor());
    rt_->setRenderPassDescriptor(rp_.get());
    REQUIRE(rt_->create());
    window_->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(rt_.get()));
    window_->setGeometry(0, 0, size.width(), size.height());
    window_->contentItem()->setSize(size);
    host_->setSize(size);
    size_ = size;
  }

  // Renders one frame and returns its RGBA8 pixels, top row first.
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
    control_.endFrame();  // an offscreen frame: waits for the GPU, so the readback is complete
    QCoreApplication::processEvents();
    REQUIRE(result.pixelSize == size_);
    return {result.data.cbegin(), result.data.cend()};
  }

  [[nodiscard]] PictureHost& host() const { return *host_; }

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

QString expected_interop() {
#if defined(__APPLE__)
  return QStringLiteral("metal-texture");
#else
  return QStringLiteral("vulkan-shared-device");
#endif
}

// The libplacebo download of `frame` rendered 1:1 by an Interop::None surface of w x h (fit = zoom 1 when the sizes
// are equal, centred).
std::vector<std::uint8_t> reference_pixels(const PictureFrame& frame, int w, int h) {
  auto surface = ovt::render::VideoSurface::create("reference");
  ovt::render::DisplayTarget target;
  target.width = w;
  target.height = h;
  REQUIRE(surface->init(target));
  surface->set_frame(frame.picture);
  ovt::render::ViewState view;
  view.zoom = 1.0;
  view.pan_x = w / 2.0;
  view.pan_y = h / 2.0;
  surface->render(view);
  INFO(surface->last_error());
  ovt::render::detail::OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(*surface, px));
  REQUIRE(px.texel_bytes == 4);
  return px.bytes;
}

// Pixels whose R, G or B differs by more than 1 (the ±1 per channel of the render PNG goldens,
// render/tests/png_golden.h): the two surfaces dither independently (ViewState::dither is on, as in the app), which
// moves single code values. The window's alpha channel is the scene graph's, not the picture's, so it is not compared.
std::size_t differing_pixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  REQUIRE(a.size() == b.size());
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

// Pixels that are not part of a grey picture: the item's background (magenta in the resize test) or the window's
// clear colour (green), which shows where the item drew nothing.
std::size_t background_pixels(const std::vector<std::uint8_t>& px) {
  std::size_t n = 0;
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    const bool magenta = px[i] == 0xFF && px[i + 1] == 0x00 && px[i + 2] == 0xFF;
    const bool green = px[i] == 0x00 && px[i + 1] == 0xFF && px[i + 2] == 0x00;
    if (magenta || green) {
      ++n;
    }
  }
  return n;
}

}  // namespace

TEST_CASE("picture_host_matches_sdr_png8_of_frame_0", "[fast][gui]") {
  if (app_or_null() == nullptr) {
    SKIP("no X display (run under xvfb-run)");
  }
  const std::shared_ptr<PictureFrame> frame = sdr709_ramp_frame0();
  const int w = static_cast<int>(frame->picture.coded_width);
  const int h = static_cast<int>(frame->picture.coded_height);
  Harness harness;
  const std::string why = harness.init(QSize(w, h));
  REQUIRE(why.empty());
  harness.host().set_picture(frame);
  std::vector<std::uint8_t> shown = harness.frame();
  harness.host().update();  // the stats reach the item in the next synchronize()
  shown = harness.frame();
  INFO("interop " << harness.host().interop().toStdString() << ", error '" << harness.host().last_error().toStdString()
                  << "'");
  REQUIRE(harness.host().last_error().isEmpty());
  REQUIRE(harness.host().interop() == expected_interop());
  REQUIRE(harness.host().frames_rendered() >= 1);

  const std::vector<std::uint8_t> reference = reference_pixels(*frame, w, h);
  const std::size_t total = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  const std::size_t differ = differing_pixels(shown, reference);
  INFO(differ << " of " << total << " pixels differ");
  CHECK(differ * 1000 <= total);  // <= 0.1 %
  // Not trivially equal: the ramp runs from black (code 64) on the left to white (code 940) on the right.
  const std::size_t mid = (static_cast<std::size_t>(h / 2) * static_cast<std::size_t>(w)) * 4;
  CHECK(shown[mid] <= 1);
  CHECK(shown[mid + (static_cast<std::size_t>(w - 1) * 4)] >= 254);
}

TEST_CASE("picture_host_resize_once_per_size_without_flicker", "[fast][gui]") {
  if (app_or_null() == nullptr) {
    SKIP("no X display (run under xvfb-run)");
  }
  const std::shared_ptr<PictureFrame> frame = sdr709_ramp_frame0();
  Harness harness;
  REQUIRE(harness.init(QSize(640, 360)).empty());
  harness.host().set_background_color(QColor(0xFF, 0x00, 0xFF));  // never part of the grey ramp
  harness.host().set_picture(frame);
  for (int i = 0; i < 3; ++i) {
    static_cast<void>(harness.frame());
  }
  REQUIRE(harness.host().last_error().isEmpty());
  CHECK(harness.host().surface_resizes() == 0);  // created at its first size, never resized

  // 16:9 sizes: the fitted ramp covers the item (up to a rounded edge column or row), so a frame that is mostly
  // background or window colour is a flicker frame.
  const std::array<QSize, 4> sizes{QSize(800, 450), QSize(1024, 576), QSize(800, 450), QSize(960, 540)};
  int expected_resizes = 0;
  for (const QSize& size : sizes) {
    harness.resize(size);
    ++expected_resizes;
    for (int i = 0; i < 3; ++i) {
      const std::vector<std::uint8_t> px = harness.frame();
      INFO("size " << size.width() << "x" << size.height() << ", frame " << i);
      const std::size_t total = static_cast<std::size_t>(size.width()) * static_cast<std::size_t>(size.height());
      CHECK(background_pixels(px) * 100 < total);
    }
    INFO("size " << size.width() << "x" << size.height());
    CHECK(harness.host().surface_resizes() == expected_resizes);  // once per size, not once per frame
    CHECK(harness.host().stats().surface_width == size.width());
    CHECK(harness.host().stats().surface_height == size.height());
  }
  CHECK(harness.host().last_error().isEmpty());
}
