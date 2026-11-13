// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of TimelineViewport and the Timeline vertex builders (WI-1.455b; chapter 11 §11.3.1): zoom 1.25x per wheel
// notch about the cursor, pan, fit, and the bars / line / band geometry.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "models/series_aggregator.h"
#include "models/timeline_geometry.h"
#include "models/timeline_viewport.h"

using Catch::Matchers::WithinAbs;
using ovt::app::Rgba;
using ovt::app::SeriesColumn;
using ovt::app::TimelineViewport;

TEST_CASE("viewport starts fitted and fits again", "[fast][gui][timeline][viewport]") {
  TimelineViewport v(1000);
  CHECK(v.is_fit());
  CHECK(v.first_frame() == 0);
  CHECK(v.end_frame() == 1000);
  v.zoom_about(4.0, 0.5);
  CHECK_FALSE(v.is_fit());
  v.fit();
  CHECK(v.is_fit());
  CHECK(v.count() == 1000.0);
}

TEST_CASE("one wheel notch zooms 1.25x about the cursor and keeps the frame under it",
          "[fast][gui][timeline][viewport]") {
  TimelineViewport v(1000);
  const double width = 800.0;
  const double x = 200.0;  // a quarter across
  const double before = v.position_at(x, width);
  v.zoom_notches(1.0, x / width);
  CHECK_THAT(v.count(), WithinAbs(1000.0 / 1.25, 1e-9));
  CHECK_THAT(v.position_at(x, width), WithinAbs(before, 1e-9));
  // Zooming out by the same notch restores the fit.
  v.zoom_notches(-1.0, x / width);
  CHECK(v.is_fit());
}

TEST_CASE("zoom is clamped to the series and to a minimum count", "[fast][gui][timeline][viewport]") {
  TimelineViewport v(100);
  v.zoom_about(1e9, 0.3);
  CHECK(v.count() == TimelineViewport::kMinCount);
  v.zoom_about(1e-9, 0.3);
  CHECK(v.count() == 100.0);
  TimelineViewport tiny(2);
  tiny.zoom_about(10.0, 0.5);
  CHECK(tiny.count() == 2.0);  // never below the series
  TimelineViewport empty;
  empty.zoom_about(2.0, 0.5);
  CHECK(empty.frame_at(10, 100) == 0);
}

TEST_CASE("pan clamps to the series", "[fast][gui][timeline][viewport]") {
  TimelineViewport v(1000);
  v.zoom_about(10.0, 0.0);  // 100 frames from 0
  v.pan_frames(250.0);
  CHECK(v.first() == 250.0);
  v.pan_frames(1e6);
  CHECK(v.first() == 900.0);
  CHECK(v.end_frame() == 1000);
  v.pan_frames(-1e6);
  CHECK(v.first() == 0.0);
}

TEST_CASE("pixel to frame mapping", "[fast][gui][timeline][viewport]") {
  TimelineViewport v(1000);
  v.zoom_about(10.0, 0.0);  // frames 0..100 over 500 px: 5 px per frame
  CHECK(v.frame_at(0.0, 500.0) == 0);
  CHECK(v.frame_at(4.9, 500.0) == 0);
  CHECK(v.frame_at(5.0, 500.0) == 1);
  CHECK(v.frame_at(499.0, 500.0) == 99);
  CHECK(v.frame_at(-50.0, 500.0) == 0);
  CHECK(v.frame_at(5000.0, 500.0) == 999);  // clamped to the series
  CHECK_THAT(v.x_of(50.0, 500.0), WithinAbs(250.0, 1e-9));
}

TEST_CASE("a new series length keeps a zoomed view and refits a fitted one", "[fast][gui][timeline][viewport]") {
  TimelineViewport v(100);
  v.set_total(200);  // was fitted: still fitted
  CHECK(v.is_fit());
  CHECK(v.count() == 200.0);
  v.zoom_about(4.0, 0.0);
  v.set_total(400);  // zoomed: kept
  CHECK(v.count() == 50.0);
  v.set_total(20);  // no longer fits: refit
  CHECK(v.count() == 20.0);
}

TEST_CASE("bars are scaled to the maximum, grow upward and skip gaps", "[fast][gui][timeline][geometry]") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<SeriesColumn> columns{{0, 1, 100.0, 100.0}, {1, 2, 50.0, 50.0}, {2, 3, nan, nan}, {3, 4, 0.0, 0.0}};
  const std::vector<Rgba> colors(columns.size(), Rgba{1, 2, 3, 255});
  const auto bars = ovt::app::build_bars(columns, colors, 100.0, 400.0F, 200.0F);
  REQUIRE(bars.size() == 12);  // two bars of six vertices; the gap and the zero draw nothing
  // First bar: x from 0 to 99 (a 1 px gap), full height.
  float min_y = 1e9F;
  float max_x = -1.0F;
  for (std::size_t i = 0; i < 6; ++i) {
    min_y = std::min(min_y, bars[i].y);
    max_x = std::max(max_x, bars[i].x);
    CHECK(bars[i].r == 1);
  }
  CHECK(min_y == 0.0F);
  CHECK(max_x == 99.0F);
  // Second bar: half height, its top at y = 100, bottom at 200.
  float top = 1e9F;
  float bottom = -1.0F;
  for (std::size_t i = 6; i < 12; ++i) {
    top = std::min(top, bars[i].y);
    bottom = std::max(bottom, bars[i].y);
  }
  CHECK(top == 100.0F);
  CHECK(bottom == 200.0F);
}

TEST_CASE("narrow columns leave no gap and a missing colour is grey", "[fast][gui][timeline][geometry]") {
  const std::vector<SeriesColumn> columns{{0, 1, 1.0, 1.0}, {1, 2, 1.0, 1.0}};
  const auto bars = ovt::app::build_bars(columns, {}, 1.0, 4.0F, 10.0F);  // 2 px per column
  REQUIRE(bars.size() == 12);
  CHECK(bars[1].x == 2.0F);  // the first bar spans its whole column
  CHECK(bars[0].r == 0x9E);
  CHECK(ovt::app::build_bars({}, {}, 1.0, 4.0F, 10.0F).empty());
  CHECK(ovt::app::build_bars(columns, {}, 0.0, 4.0F, 10.0F).empty());
}

TEST_CASE("the mean line joins neighbouring non-gap columns", "[fast][gui][timeline][geometry]") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<SeriesColumn> columns{
      {0, 1, 4.0, 2.0}, {1, 2, 4.0, 4.0}, {2, 3, nan, nan}, {3, 4, 4.0, 4.0}, {4, 5, 4.0, 0.0}};
  const auto line = ovt::app::build_mean_line(columns, 4.0, 50.0F, 40.0F, Rgba{9, 9, 9, 255});
  REQUIRE(line.size() == 4);  // 0-1 and 3-4; the gap breaks the line
  CHECK(line[0].x == 5.0F);   // centre of column 0 (10 px wide)
  CHECK(line[0].y == 20.0F);  // mean 2 of 4
  CHECK(line[1].y == 0.0F);
  CHECK(line[3].y == 40.0F);
}

TEST_CASE("frame type colours follow section 11.4.5", "[fast][gui][timeline][geometry]") {
  using ovt::app::frame_type_color;
  namespace fb = ovt::fb;
  CHECK(frame_type_color(fb::FrameType::I, true) == Rgba{0xE5, 0x39, 0x35, 255});
  CHECK(frame_type_color(fb::FrameType::Key, true) == frame_type_color(fb::FrameType::I, true));
  CHECK(frame_type_color(fb::FrameType::P, true) == Rgba{0x1E, 0x88, 0xE5, 255});
  CHECK(frame_type_color(fb::FrameType::B, true) == Rgba{0x43, 0xA0, 0x47, 255});
  CHECK(frame_type_color(fb::FrameType::Switch, true) == Rgba{0x9E, 0x9E, 0x9E, 255});
  CHECK(frame_type_color(fb::FrameType::P, false).a < 255);  // hidden frame
  const std::vector<SeriesColumn> columns{{0, 1, 3.0, 3.0}, {1, 2, 7.0, 7.0}};
  CHECK(ovt::app::max_of(columns) == 7.0);
  CHECK(ovt::app::max_of({}) == 0.0);
  const auto rect = ovt::app::build_rect(1.0F, 2.0F, 3.0F, 4.0F, Rgba{1, 1, 1, 1});
  CHECK(rect.size() == 6);
}
