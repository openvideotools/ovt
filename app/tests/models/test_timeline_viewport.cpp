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
  CHECK(frame_type_color(fb::FrameType::I) == Rgba{0xE5, 0x39, 0x35, 255});
  CHECK(frame_type_color(fb::FrameType::Key) == frame_type_color(fb::FrameType::I));
  CHECK(frame_type_color(fb::FrameType::P) == Rgba{0x1E, 0x88, 0xE5, 255});
  CHECK(frame_type_color(fb::FrameType::B) == Rgba{0x43, 0xA0, 0x47, 255});
  CHECK(frame_type_color(fb::FrameType::Switch) == Rgba{0x9E, 0x9E, 0x9E, 255});
  const std::vector<SeriesColumn> columns{{0, 1, 3.0, 3.0}, {1, 2, 7.0, 7.0}};
  CHECK(ovt::app::max_of(columns) == 7.0);
  CHECK(ovt::app::max_of({}) == 0.0);
  const auto rect = ovt::app::build_rect(1.0F, 2.0F, 3.0F, 4.0F, Rgba{1, 1, 1, 1});
  CHECK(rect.size() == 6);
}

TEST_CASE("the hatch of a hidden frame is clipped to its bar", "[fast][gui][timeline][geometry]") {
  const auto lines = ovt::app::build_hatch(10.0F, 0.0F, 30.0F, 10.0F, Rgba{0, 0, 0, 255}, 4.0F);
  REQUIRE(!lines.empty());
  CHECK(lines.size() % 2 == 0);
  for (const ovt::app::ColorVertex& v : lines) {
    CHECK(v.x >= 10.0F);
    CHECK(v.x <= 30.0F);
    CHECK(v.y >= 0.0F);
    CHECK(v.y <= 10.0F);
  }
  // Every line rises one pixel per pixel to the right.
  for (std::size_t i = 0; i + 1 < lines.size(); i += 2) {
    CHECK(lines[i + 1].x - lines[i].x == lines[i].y - lines[i + 1].y);
  }
  CHECK(ovt::app::build_hatch(10.0F, 5.0F, 30.0F, 5.0F, Rgba{}).empty());  // no height
  CHECK(ovt::app::build_hatch(30.0F, 0.0F, 10.0F, 5.0F, Rgba{}).empty());  // no width
}

TEST_CASE("reference arcs are parabolas capped in height; ticks are 2 px bars", "[fast][gui][timeline][geometry]") {
  const std::vector<ovt::app::RefArc> arcs{{100.0F, 60.0F, Rgba{1, 2, 3, 4}}, {0.0F, 400.0F, Rgba{}}};
  const auto lines = ovt::app::build_ref_arcs(arcs, 100.0F, 50.0F);
  REQUIRE(lines.size() == 64U);  // 16 segments, two vertices each, two arcs
  CHECK(lines.front().x == 100.0F);
  CHECK(lines.front().y == 100.0F);
  CHECK(lines[31].x == 60.0F);  // ends at the target, back on the baseline
  CHECK(lines[31].y == 100.0F);
  CHECK(lines[0].r == 1);
  float top_short = 1e9F;
  float top_long = 1e9F;
  for (std::size_t i = 0; i < 32; ++i) {
    top_short = std::min(top_short, lines[i].y);
  }
  for (std::size_t i = 32; i < lines.size(); ++i) {
    top_long = std::min(top_long, lines[i].y);
  }
  CHECK_THAT(top_short, WithinAbs(80.0, 1e-3));  // half the 40 px distance
  CHECK_THAT(top_long, WithinAbs(50.0, 1e-3));   // capped
  const std::vector<float> xs{10.0F, 20.0F};
  const auto ticks = ovt::app::build_ticks(xs, 100.0F, 6.0F, Rgba{9, 9, 9, 255});
  REQUIRE(ticks.size() == 12);
  CHECK(ticks[0].x == 9.0F);
  CHECK(ticks[0].y == 94.0F);
}
