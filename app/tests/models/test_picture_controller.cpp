// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of PictureController (WI-1.456; chapter 11 §11.3.2, §11.4.4): zoom about the cursor (AT1), zoom levels and
// clamping, pan clamping, plane actions (AT3), probe pin/unpin into Selection (AT2, model half), the selection
// rectangle and Esc. The QML half (input events, crosshair) is app/tests/qml/tst_picture.qml.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QJSEngine>
#include <QPoint>
#include <QRect>
#include <QSignalSpy>

#include <array>
#include <cmath>
#include <cstddef>

#include "models/qt_test_app.h"
#include "models/selection_model.h"
#include "picture_controller.h"

using Catch::Matchers::WithinAbs;
using ovt::app::PictureController;
using ovt::app::SelectionModel;

namespace {

// An 800 x 600 device-pixel viewport showing a 1920 x 1080 picture.
void setup(PictureController& c) {
  c.set_viewport_width(800);
  c.set_viewport_height(600);
  c.setPicture(1920, 1080);
}

}  // namespace

TEST_CASE("a new picture is fitted and centred like PictureHost's zoom 0", "[fast][gui][picture]") {
  ovt::app::testing::qt_app();
  PictureController c;
  setup(c);
  CHECK(c.fit());
  CHECK_THAT(c.zoom(), WithinAbs(800.0 / 1920.0, 1e-12));
  CHECK_THAT(c.pan_x(), WithinAbs(960.0, 1e-12));
  CHECK_THAT(c.pan_y(), WithinAbs(540.0, 1e-12));
  // The crop rectangle (conformance window) is what fits while shown cropped.
  c.setPicture(1920, 1088, QRect(0, 0, 1920, 1080));
  CHECK_THAT(c.pan_y(), WithinAbs(540.0, 1e-12));
  c.set_show_cropped(false);
  c.fitToView();
  CHECK_THAT(c.pan_y(), WithinAbs(544.0, 1e-12));
}

TEST_CASE("AT1: one wheel notch at device (400,300) from zoom 1 gives zoom 2 and keeps the point under the cursor",
          "[fast][gui][picture][at1]") {
  ovt::app::testing::qt_app();
  PictureController c;
  setup(c);
  c.zoomTo(1.0);
  REQUIRE(c.zoom() == 1.0);
  // Off-centre too, so the test does not pass merely because the cursor sits at the viewport centre.
  for (const QPointF at : {QPointF(400, 300), QPointF(123, 457), QPointF(799, 1)}) {
    c.zoomTo(1.0);
    const QPoint before = c.pick(at.x(), at.y());
    const QPointF before_f = c.imagePoint(at.x(), at.y());
    c.wheel(at.x(), at.y(), 120);
    CHECK(c.zoom() == 2.0);
    CHECK(c.pick(at.x(), at.y()) == before);
    const QPointF after_f = c.imagePoint(at.x(), at.y());
    CHECK_THAT(after_f.x(), WithinAbs(before_f.x(), 1e-9));
    CHECK_THAT(after_f.y(), WithinAbs(before_f.y(), 1e-9));
  }
}

TEST_CASE("smooth-scrolling deltas accumulate to x2 per notch; zoom stays within 1/32..64", "[fast][gui][picture]") {
  ovt::app::testing::qt_app();
  PictureController c;
  setup(c);
  c.zoomTo(1.0);
  for (int i = 0; i < 8; ++i) {
    c.wheel(300, 200, 15);
  }
  CHECK_THAT(c.zoom(), WithinAbs(2.0, 1e-12));
  c.wheel(300, 200, -240);
  CHECK_THAT(c.zoom(), WithinAbs(0.5, 1e-12));
  for (int i = 0; i < 20; ++i) {
    c.wheel(300, 200, 120);
  }
  CHECK(c.zoom() == PictureController::kMaxZoom);
  for (int i = 0; i < 40; ++i) {
    c.wheel(300, 200, -120);
  }
  CHECK(c.zoom() == PictureController::kMinZoom);
}

TEST_CASE("+ and - step through powers of two about the centre; Ctrl+1/2/4 and R", "[fast][gui][picture]") {
  ovt::app::testing::qt_app();
  PictureController c;
  setup(c);  // fit = 0.4166…
  const QPointF centre = c.imagePoint(400, 300);
  CHECK(c.handleAction(QStringLiteral("view.zoom.in")));
  CHECK(c.zoom() == 0.5);
  CHECK_FALSE(c.fit());
  CHECK(c.handleAction(QStringLiteral("view.zoom.in")));
  CHECK(c.zoom() == 1.0);
  CHECK(c.handleAction(QStringLiteral("view.zoom.out")));
  CHECK(c.zoom() == 0.5);
  CHECK_THAT(c.imagePoint(400, 300).x(), WithinAbs(centre.x(), 1e-9));
  CHECK(c.handleAction(QStringLiteral("view.zoom.400")));
  CHECK(c.zoom() == 4.0);
  CHECK(c.handleAction(QStringLiteral("view.zoom.200")));
  CHECK(c.zoom() == 2.0);
  CHECK(c.handleAction(QStringLiteral("view.zoom.100")));
  CHECK(c.zoom() == 1.0);
  CHECK(c.handleAction(QStringLiteral("view.zoom.fit")));
  CHECK(c.fit());
  CHECK_THAT(c.zoom(), WithinAbs(800.0 / 1920.0, 1e-12));
  CHECK_FALSE(c.handleAction(QStringLiteral("view.theme.dark")));
}

TEST_CASE("panning follows the pointer and clamps so 64 device px of picture stay visible", "[fast][gui][picture]") {
  ovt::app::testing::qt_app();
  PictureController c;
  setup(c);
  c.zoomTo(1.0);
  const double pan_x = c.pan_x();
  c.beginPan(100, 100);
  c.movePan(150, 120);
  c.endPan();
  CHECK_THAT(c.pan_x(), WithinAbs(pan_x - 50.0, 1e-9));
  // Drag the picture far to the right: its left edge stops 64 px before the viewport's right edge.
  c.panBy(100000, 0);
  CHECK_THAT(c.devicePoint(0, 0).x(), WithinAbs(800.0 - PictureController::kMinVisible, 1e-9));
  // And far up: its bottom edge stops 64 px below the viewport's top.
  c.panBy(0, -100000);
  CHECK_THAT(c.devicePoint(0, 1080).y(), WithinAbs(PictureController::kMinVisible, 1e-9));
  // A tiny picture (smaller than 64 px on screen) stays wholly visible.
  c.setPicture(16, 16);
  c.zoomTo(1.0);
  c.panBy(100000, 100000);
  CHECK(c.devicePoint(16, 16).x() <= 800.0 + 1e-9);
  CHECK(c.devicePoint(0, 0).x() >= -1e-9);
}

TEST_CASE("the same picture size keeps zoom and pan; a new size refits", "[fast][gui][picture]") {
  ovt::app::testing::qt_app();
  PictureController c;
  setup(c);
  c.zoomTo(2.0);
  c.panBy(30, 40);
  const double pan_x = c.pan_x();
  c.setPicture(1920, 1080);  // the next frame
  CHECK(c.zoom() == 2.0);
  CHECK(c.pan_x() == pan_x);
  c.setPicture(1280, 720);
  CHECK(c.fit());
  c.setPicture(0, 0);
  CHECK_FALSE(c.has_picture());
  CHECK(c.pick(400, 300) == QPoint(-1, -1));
}

TEST_CASE("AT3: plane actions 1-5 set plane_mode 0-4", "[fast][gui][picture][at3]") {
  ovt::app::testing::qt_app();
  PictureController c;
  const QSignalSpy spy(&c, &PictureController::planeChanged);
  const std::array<const char*, 5> ids = {"view.plane.yuv", "view.plane.y", "view.plane.u", "view.plane.v", "view.plane.gray"};
  for (int i = 4; i >= 0; --i) {
    CHECK(c.handleAction(QLatin1String(ids.at(static_cast<std::size_t>(i)))));
    CHECK(c.plane_mode() == i);
  }
  CHECK(spy.count() == 5);  // 0 → 4 → 3 → 2 → 1 → 0
  c.set_plane_mode(9);
  CHECK(c.plane_mode() == 4);
}

TEST_CASE("AT2 (model): Ctrl+click pins the probe in Selection, again unpins; Esc unpins",
          "[fast][gui][picture][at2]") {
  ovt::app::testing::qt_app();
  SelectionModel sel;
  PictureController c;
  c.set_selection(&sel);
  setup(c);
  c.zoomTo(1.0);
  const QSignalSpy probe_spy(&c, &PictureController::probeChanged);
  const QPoint at = c.pick(410.5, 290.5);
  c.toggleProbe(410.5, 290.5);
  CHECK(sel.probe() == at);
  CHECK(c.probe() == at);
  CHECK(sel.origin() == QStringLiteral("picture"));
  CHECK(probe_spy.count() == 1);
  // The pinned probe is in image coordinates, so it survives panning.
  c.panBy(37, -12);
  CHECK(sel.probe() == at);
  CHECK(c.devicePoint(at.x(), at.y()).x() != 410.0);
  // Probe arrows move it by one sample.
  c.moveProbe(1, 0);
  CHECK(sel.probe() == at + QPoint(1, 0));
  const QPointF dev = c.devicePoint(at.x() + 1.5, at.y() + 0.5);
  c.toggleProbe(dev.x(), dev.y());
  CHECK(sel.probe() == QPoint(-1, -1));
  c.toggleProbe(dev.x(), dev.y());
  CHECK(c.escape());
  CHECK(sel.probe() == QPoint(-1, -1));
  CHECK_FALSE(c.escape());
  // Outside the picture nothing is pinned.
  c.fitToView();
  c.toggleProbe(400, 2);  // letterbox above a 16:9 picture in a 4:3 viewport
  CHECK(sel.probe() == QPoint(-1, -1));
}

TEST_CASE("Shift+drag draws a cell-snapped rectangle that is committed to Selection; Esc clears",
          "[fast][gui][picture]") {
  ovt::app::testing::qt_app();
  SelectionModel sel;
  PictureController c;
  c.set_selection(&sel);
  setup(c);
  c.zoomTo(1.0);
  const QPointF a = c.devicePoint(13.2, 21.7);
  const QPointF b = c.devicePoint(5.1, 30.0);
  c.beginRect(a.x(), a.y());
  c.moveRect(b.x(), b.y());
  CHECK(c.drag_rect() == QRect(4, 20, 12, 12));  // x 4..16, y 20..32
  CHECK(sel.selection_rect().isNull());
  c.endRect();
  CHECK(c.drag_rect().isNull());
  CHECK(sel.selection_rect() == QRect(4, 20, 12, 12));
  // Clamped to the coded picture.
  c.beginRect(c.devicePoint(1910, 1070).x(), c.devicePoint(1910, 1070).y());
  c.moveRect(c.devicePoint(2500, 1500).x(), c.devicePoint(2500, 1500).y());
  CHECK(c.drag_rect() == QRect(1908, 1068, 12, 12));
  // Esc while drawing cancels the drawing only.
  CHECK(c.escape());
  CHECK(c.drag_rect().isNull());
  CHECK(sel.selection_rect() == QRect(4, 20, 12, 12));
  CHECK(c.escape());
  CHECK(sel.selection_rect().isNull());
}

TEST_CASE("the read-out shows position, code values, zoom and plane", "[fast][gui][picture]") {
  ovt::app::testing::qt_app();
  QJSEngine engine;
  SelectionModel sel;
  PictureController c;
  c.set_selection(&sel);
  setup(c);
  c.zoomTo(1.0);
  c.set_sample_reader(engine.evaluate(QStringLiteral("(function (x, y) { return [x % 256, 128, y % 256] })")));
  const QPointF at = c.devicePoint(300.5, 200.5);
  c.hoverAt(at.x(), at.y());
  CHECK(c.hover() == QPoint(300, 200));
  CHECK(c.readout() == QStringLiteral("x 300, y 200 · Y 44 U 128 V 200 · 100 % · YUV"));
  c.set_plane_mode(1);
  CHECK(c.readout().endsWith(QStringLiteral("· Y")));
  // Zooming moves the picture under the resting pointer: the hovered sample follows.
  c.zoomAbout(2.0, 0, 0);
  CHECK(c.hover() == c.pick(at.x(), at.y()));
  c.leave();
  c.toggleProbe(at.x(), at.y());
  CHECK(c.readout().startsWith(QStringLiteral("Probe x ")));
  c.fitToView();
  CHECK(c.readout().contains(QStringLiteral("Fit 41.7 %")) == false);  // 41.666… % rounds to a whole number
  CHECK(c.readout().contains(QStringLiteral("Fit 42 %")));
}

TEST_CASE("a frame change in Selection asks for that frame", "[fast][gui][picture]") {
  ovt::app::testing::qt_app();
  SelectionModel sel;
  PictureController c;
  c.set_selection(&sel);
  const QSignalSpy spy(&c, &PictureController::frameWanted);
  sel.selectFrame(7, -1, QStringLiteral("timeline"));
  REQUIRE(spy.count() == 1);
  CHECK(spy.at(0).at(0).toInt() == 7);
}
