// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.463 (chapter 11 §11.10): NativeSurfaceHost on the offscreen platform with a test backend (a raster child
// window and a fake native handle), so it runs on every CI platform.
//
//   AT1, automated part: during a 100-step splitter drag the child window's rect equals the PictureHost item's rect
//   +-1 px, and the NATIVE target's drawable is that rect times the devicePixelRatio. The other half of AT1
//   (Ctrl+H on an XDR Mac gives active_encoding() == ScRgbLinear16F) needs the display: render_test_macos_metal covers
//   the surface side (MetalTexture surface -> CAMetalLayer -> ScRgbLinear16F) and docs/acceptance/wi/WI-1.463.md
//   holds the deferred manual record.
//   Steps 1 and 3: child window lifecycle, EMBEDDED <-> NATIVE switching, and the popup rule.

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QSignalSpy>
#include <QSurface>
#include <QWindow>
#include <QtTest/QTest>

#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>

#include "native_surface_host.h"

namespace {

using ovt::app::NativeSurfaceHost;
using ovt::app::NativeWindowBackend;

// The application, once, on the offscreen platform with the software scene graph (no GPU needed).
QGuiApplication& app() {
  static QGuiApplication* instance = [] {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
      qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    qputenv("QT_SCALE_FACTOR", "1");
    QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));
    static int argc = 1;
    static std::array<char, 26> name{"ovt_tests_app_native_host"};
    static std::array<char*, 2> argv{name.data(), nullptr};
    return new QGuiApplication(argc, argv.data());  // NOLINT(cppcoreguidelines-owning-memory): lives until exit
  }();
  return *instance;
}

// A raster child window and a fixed "layer" pointer; counts how often the handle was asked for.
struct TestBackend final : NativeWindowBackend {
  int* handle_calls = nullptr;
  bool fail = false;
  static int layer;  // what DisplayTarget::native points at

  [[nodiscard]] QSurface::SurfaceType surface_type() const override { return QSurface::RasterSurface; }
  [[nodiscard]] int target_kind() const override { return 1; }  // DisplayTarget::Kind::MetalLayer
  [[nodiscard]] void* native_handle(QWindow& window, QString& error) override {
    if (handle_calls != nullptr) {
      ++*handle_calls;
    }
    if (fail || window.winId() == 0) {
      error = QStringLiteral("test backend: no handle");
      return nullptr;
    }
    return &layer;
  }
};
int TestBackend::layer = 0;

// Main window 800x600 with a horizontal "SplitView": left pane | 6 px handle | right pane holding the picture item
// below a 30 px header and inside 10 px margins (the Picture panel's canvas), and an overlay item filling the window
// (Overlay.overlay).
struct Scene {
  QGuiApplication& application = app();  // before the window
  QQuickWindow window;
  QQuickItem* left = nullptr;
  QQuickItem* right = nullptr;
  QQuickItem* picture = nullptr;
  QQuickItem* overlay = nullptr;

  Scene() {
    window.setGeometry(0, 0, 800, 600);
    QQuickItem* root = window.contentItem();
    root->setSize(QSizeF(800, 600));
    left = new QQuickItem(root);      // NOLINT(cppcoreguidelines-owning-memory): owned by the item tree
    right = new QQuickItem(root);     // NOLINT(cppcoreguidelines-owning-memory)
    picture = new QQuickItem(right);  // NOLINT(cppcoreguidelines-owning-memory)
    overlay = new QQuickItem(root);   // NOLINT(cppcoreguidelines-owning-memory)
    overlay->setSize(QSizeF(800, 600));
    overlay->setZ(1000);
    drag(240.0);
    picture->setPosition(QPointF(10.0, 30.0));
  }

  // The splitter handle at `x`: the panes and the picture inside the right one follow.
  void drag(double x) const {
    left->setSize(QSizeF(x, 600.0));
    right->setPosition(QPointF(x + 6.0, 0.0));
    right->setSize(QSizeF(800.0 - x - 6.0, 600.0));
    picture->setSize(QSizeF(right->width() - 20.0, 600.0 - 30.0 - 24.0));
  }

  void show() {
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
  }

  // The picture's rect in the window, computed independently of the host.
  [[nodiscard]] QRectF expected() const {
    return {right->x() + picture->x(), right->y() + picture->y(), picture->width(), picture->height()};
  }
};

bool within_one(const QRect& got, const QRectF& want) {
  return std::abs(got.x() - want.x()) <= 1.0 && std::abs(got.y() - want.y()) <= 1.0 &&
         std::abs(got.right() + 1 - want.right()) <= 1.0 && std::abs(got.bottom() + 1 - want.bottom()) <= 1.0;
}

std::unique_ptr<NativeSurfaceHost> make_host(Scene& scene, int* handle_calls = nullptr, bool fail = false) {
  auto backend = std::make_unique<TestBackend>();
  backend->handle_calls = handle_calls;
  backend->fail = fail;
  auto host = std::make_unique<NativeSurfaceHost>(std::move(backend), nullptr);
  host->set_item(scene.picture);
  host->set_overlay(scene.overlay);
  host->set_hdr_capable(true);
  host->set_headroom(4.0);
  return host;
}

}  // namespace

TEST_CASE("WI-1.463 AT1 (geometry): the child window follows the item +-1 px during a 100-step splitter drag",
          "[fast][gui][native]") {
  Scene scene;
  scene.show();
  auto host = make_host(scene);
  host->set_requested(true);  // Ctrl+H
  REQUIRE(host->native());
  QWindow* child = host->childWindow();
  REQUIRE(child != nullptr);
  CHECK(child->parent() == &scene.window);
  CHECK(child->isVisible());
  const double dpr = scene.window.devicePixelRatio();
  for (int step = 0; step < 100; ++step) {
    // Fractional positions as a pointer drag produces them.
    scene.drag(240.0 + (step * 3.37));
    const QRectF want = scene.expected();
    INFO("step " << step);
    CHECK(within_one(child->geometry(), want));
    CHECK(within_one(host->geometry(), want));
    CHECK(host->target().native);
    CHECK(host->target().width == static_cast<int>(std::lround(host->geometry().width() * dpr)));
    CHECK(host->target().height == static_cast<int>(std::lround(host->geometry().height() * dpr)));
  }
  // The picture item moved inside its pane (a header appearing) is followed too.
  scene.picture->setY(60.0);
  CHECK(within_one(child->geometry(), scene.expected()));
}

TEST_CASE("WI-1.463 step 1: EMBEDDED until requested on an HDR-capable display; one child window, reused",
          "[fast][gui][native]") {
  Scene scene;
  scene.show();
  int handle_calls = 0;
  auto host = make_host(scene, &handle_calls);
  CHECK(host->available());
  CHECK_FALSE(host->native());
  CHECK(host->childWindow() == nullptr);  // nothing created while EMBEDDED
  CHECK_FALSE(host->target().native);

  const QSignalSpy targets(host.get(), &NativeSurfaceHost::targetChanged);
  host->set_requested(true);
  REQUIRE(host->native());
  CHECK(targets.count() == 1);
  const ovt::app::NativeTargetDesc native = host->target();
  CHECK(native.handle == &TestBackend::layer);
  CHECK(native.kind == 1);
  CHECK(native.hdr_capable);
  CHECK(native.headroom == 4.0);
  CHECK(native.encoding == 0);  // ScRgbLinear16F, the macOS default (§10.4.1)
  QWindow* child = host->childWindow();
  CHECK((child->flags() & Qt::WindowTransparentForInput) != 0);  // the panel's MouseArea keeps the input

  host->set_encoding(1);  // "System PQ pass-through"
  CHECK(host->target().encoding == 1);
  CHECK(host->target().generation > native.generation);

  host->set_requested(false);
  CHECK_FALSE(host->native());
  CHECK_FALSE(host->target().native);
  CHECK(host->childWindow() == child);  // hidden, not destroyed
  CHECK_FALSE(child->isVisible());

  host->set_requested(true);
  CHECK(host->native());
  CHECK(host->childWindow() == child);
  CHECK(child->isVisible());
  CHECK(handle_calls == 1);

  // An SDR display (DisplayCaps.hdr_capable false): EMBEDDED whatever is requested (R-ARCH-2).
  host->set_hdr_capable(false);
  CHECK_FALSE(host->native());
  CHECK_FALSE(child->isVisible());
}

TEST_CASE("WI-1.463 step 1: no backend (Linux in phase 1) stays EMBEDDED", "[fast][gui][native]") {
  Scene scene;
  scene.show();
  NativeSurfaceHost host(nullptr, nullptr);
  host.set_item(scene.picture);
  host.set_hdr_capable(true);
  host.set_requested(true);
  CHECK_FALSE(host.available());
  CHECK_FALSE(host.native());
  CHECK(host.childWindow() == nullptr);
  CHECK(host.target().generation == 0);
}

TEST_CASE("WI-1.463 step 1: the child window is created only once the parent window is exposed",
          "[fast][gui][native]") {
  Scene scene;  // not shown yet
  auto host = make_host(scene);
  host->set_requested(true);
  CHECK_FALSE(host->native());
  CHECK(host->childWindow() == nullptr);
  scene.show();
  host->retarget();
  CHECK(host->native());
  CHECK(host->childWindow() != nullptr);
}

TEST_CASE("WI-1.463 step 1: a failed native handle or retarget goes back to EMBEDDED until the request changes",
          "[fast][gui][native]") {
  Scene scene;
  scene.show();
  SECTION("the platform gives no handle") {
    auto host = make_host(scene, nullptr, /*fail=*/true);
    host->set_requested(true);
    CHECK_FALSE(host->native());
    CHECK(host->error() == QStringLiteral("test backend: no handle"));
  }
  SECTION("VideoSurface::retarget() reported RND-0002") {
    auto host = make_host(scene);
    host->set_requested(true);
    REQUIRE(host->native());
    host->reportNativeFailure(QStringLiteral("RND-0002 renderer init failed: swapchain: test"));
    CHECK_FALSE(host->native());
    CHECK_FALSE(host->childWindow()->isVisible());
    CHECK(host->error().startsWith(QStringLiteral("RND-0002")));
    scene.drag(300.0);  // a geometry change does not retry
    CHECK_FALSE(host->native());
    host->set_requested(false);
    host->set_requested(true);  // Ctrl+H twice retries
    CHECK(host->native());
    CHECK(host->error().isEmpty());
  }
}

TEST_CASE("WI-1.463 step 3: the popup rule hides the child window while a popup covers the picture",
          "[fast][gui][native]") {
  Scene scene;
  scene.show();
  auto host = make_host(scene);
  host->set_requested(true);
  REQUIRE(host->native());
  QWindow* child = host->childWindow();

  // A menu dropped over the picture (a popup item in Overlay.overlay).
  auto* menu = new QQuickItem(scene.overlay);  // NOLINT(cppcoreguidelines-owning-memory): owned by the overlay
  menu->setPosition(QPointF(400.0, 100.0));
  menu->setSize(QSizeF(160.0, 200.0));
  CHECK(host->popup_open());
  CHECK(host->suppressed());
  CHECK_FALSE(host->native());
  CHECK_FALSE(child->isVisible());
  CHECK_FALSE(host->target().native);

  menu->setVisible(false);  // closed
  CHECK_FALSE(host->popup_open());
  CHECK(host->native());
  CHECK(child->isVisible());

  // A tooltip over the toolbar, outside the picture, leaves it alone.
  menu->setVisible(true);
  menu->setPosition(QPointF(400.0, 0.0));
  menu->setSize(QSizeF(80.0, 20.0));
  CHECK_FALSE(host->popup_open());
  CHECK(host->native());

  // A modal dialog's dimmer covers the whole window.
  auto* dimmer = new QQuickItem(scene.overlay);  // NOLINT(cppcoreguidelines-owning-memory)
  dimmer->setSize(QSizeF(800.0, 600.0));
  CHECK(host->popup_open());
  CHECK_FALSE(host->native());
  delete dimmer;  // NOLINT(cppcoreguidelines-owning-memory): the dialog closed and its dimmer went
  CHECK_FALSE(host->popup_open());
  CHECK(host->native());
}

TEST_CASE("WI-1.463 step 3: hidden panel or setVisible(false) hides the child window", "[fast][gui][native]") {
  Scene scene;
  scene.show();
  auto host = make_host(scene);
  host->set_requested(true);
  REQUIRE(host->native());
  QWindow* child = host->childWindow();

  scene.right->setVisible(false);  // the Picture's tab is not current
  CHECK_FALSE(host->native());
  CHECK_FALSE(child->isVisible());
  scene.right->setVisible(true);
  CHECK(host->native());

  host->setVisible(false);
  CHECK_FALSE(host->native());
  CHECK(host->suppressed());
  host->setVisible(true);
  CHECK(host->native());
}

TEST_CASE("WI-1.463: NATIVE mode asks for a render every 500 ms (EDR headroom poll, §10.4.1)", "[fast][gui][native]") {
  Scene scene;
  scene.show();
  auto host = make_host(scene);
  QSignalSpy refresh(host.get(), &NativeSurfaceHost::refreshRequested);
  host->set_requested(true);
  REQUIRE(host->native());
  CHECK(refresh.wait(2000));
  host->set_requested(false);
  refresh.clear();
  QTest::qWait(700);
  CHECK(refresh.count() == 0);
}
