// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.409 (chapter 10 §10.11): zoom, pan, nearest-neighbour magnification and the sample grid (§10.2.3, §10.6.2,
// R-ZOOM-1..4), with the golden grid_zoom8.png (tests/golden/render/, png_io.h). The transform tests are CPU only.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "ovt/render/video_surface.h"
#include "pipeline/render_params.h"
#include "pipeline/view_transform.h"
#include "png_io.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::ViewState;
using ovt::render::detail::device_to_image;
using ovt::render::detail::image_to_device;
using ovt::render::detail::OutputPixels;
using ovt::render::detail::sample_at_device;
using ovt::render::detail::ViewportSize;
using ovt::render::test::OwnedPicture;
using ovt::render::test::RgbImage;

ViewState view_of(double zoom, double pan_x, double pan_y) {
  ViewState v;
  v.zoom = zoom;
  v.pan_x = pan_x;
  v.pan_y = pan_y;
  return v;
}

// §10.9.2 golden rule (#465, #469): a pixel differs when any 8-bit channel is more than 2 codes off, and at most 0.1 %
// of the pixels may differ. OVT_UPDATE_GOLDEN=1 rewrites tests/golden/render/<name> instead of comparing.
void check_golden(const std::string& name, const RgbImage& actual) {
  const std::string path = std::string(OVT_RENDER_GOLDEN_DIR) + "/" + name;
  const char* update = std::getenv("OVT_UPDATE_GOLDEN");  // NOLINT(concurrency-mt-unsafe): test setup
  if (update != nullptr && std::strcmp(update, "1") == 0) {
    REQUIRE(ovt::render::test::write_png(path, actual, true));
    WARN("rewrote golden " << path);
    return;
  }
  const std::optional<RgbImage> read = ovt::render::test::read_png(path);
  INFO("golden " << path);
  REQUIRE(read.has_value());
  const RgbImage golden = read.value_or(RgbImage{});
  REQUIRE(golden.width == actual.width);
  REQUIRE(golden.height == actual.height);
  const size_t pixels = static_cast<size_t>(actual.width) * static_cast<size_t>(actual.height);
  size_t differing = 0;
  for (size_t p = 0; p < pixels; ++p) {
    bool differs = false;
    for (size_t c = 0; c < 3; ++c) {
      differs = differs || std::abs(int{golden.rgb.at((p * 3) + c)} - int{actual.rgb.at((p * 3) + c)}) > 2;
    }
    differing += differs ? 1 : 0;
  }
  INFO(differing << " of " << pixels << " pixels differ by more than 2 codes");
  CHECK(differing * 1000 <= pixels);
}

}  // namespace

// Acceptance test 1.
TEST_CASE("render_test_view_transform: zoom 4, pan (100, 50), viewport 800x600 maps device pixels to samples (AT1)",
          "[fast][render]") {
  const ViewState v = view_of(4.0, 100.0, 50.0);
  const ViewportSize vp{800, 600};
  const auto centre = device_to_image(v, vp, 400, 300);
  CHECK(centre.x == 100.0);
  CHECK(centre.y == 50.0);
  const auto next = device_to_image(v, vp, 404, 300);
  CHECK(next.x == 101.0);
  CHECK(next.y == 50.0);
  CHECK(sample_at_device(v, vp, 400, 300).x == 100);
  CHECK(sample_at_device(v, vp, 404, 300).x == 101);
  CHECK(sample_at_device(v, vp, 403, 300).x == 100);
  // The forward mapping returns where each sample starts.
  CHECK(image_to_device(v, vp, 100.0, 50.0).x == 400.0);
  CHECK(image_to_device(v, vp, 101.0, 50.0).x == 404.0);
}

TEST_CASE("render_test_view_transform: the inverse uses the forward transform's rounding", "[fast][render]") {
  // A fractional zoom with a fractional pan: snapped sample edges are uneven, and the sample a device pixel selects is
  // still the one whose snapped footprint contains it.
  const ViewportSize vp{97, 61};
  for (const double zoom : {1.0, 1.5, 2.25, 3.0, 7.7, 8.0, 13.3, 24.0}) {
    const ViewState v = view_of(zoom, 10.37, 5.81);
    for (int px = 0; px < vp.w; ++px) {
      const int s = sample_at_device(v, vp, px, 0).x;
      const double lo = image_to_device(v, vp, s, 0).x;
      const double hi = image_to_device(v, vp, s + 1, 0).x;
      INFO("zoom " << zoom << " device " << px << " sample " << s << " [" << lo << ", " << hi << ")");
      REQUIRE(lo <= px);
      REQUIRE(px < hi);
    }
  }
  // Below zoom 1 nothing is snapped: the forward transform is the plain §10.2.3 formula.
  const ViewState half = view_of(0.5, 10.0, 10.0);
  CHECK(image_to_device(half, vp, 11.0, 11.0).x == ((11.0 - 10.0) * 0.5) + 48.5);
}

TEST_CASE("render_test_view_transform: target.crop is snapped to whole device pixels at zoom >= 1", "[fast][render]") {
  using ovt::render::detail::target_rect;
  const ViewState v = view_of(3.0, 10.4, 5.5);
  const pl_rect2df r = target_rect(pl_rect2df{0, 0, 20, 10}, v, 101, 51);
  for (const float e : {r.x0, r.y0, r.x1, r.y1}) {
    CHECK(e == std::floor(e));
  }
  // Every sample is then exactly three device pixels wide.
  CHECK(r.x1 - r.x0 == 60.0F);
  CHECK(r.y1 - r.y0 == 30.0F);
  const ViewState small = view_of(0.5, 10.4, 5.5);
  const pl_rect2df s = target_rect(pl_rect2df{0, 0, 20, 10}, small, 101, 51);
  CHECK(s.x0 != std::floor(s.x0));  // fractional below zoom 1
}

TEST_CASE("render_test_view_transform: zoom thresholds", "[fast][render]") {
  using ovt::render::detail::sample_grid_visible;
  using ovt::render::detail::snap_device;
  using ovt::render::detail::zoom_filter;
  using ovt::render::detail::ZoomFilter;
  CHECK(zoom_filter(0.99) == ZoomFilter::Downscale);
  CHECK(zoom_filter(1.0) == ZoomFilter::Nearest);
  CHECK(zoom_filter(64.0) == ZoomFilter::Nearest);
  CHECK_FALSE(sample_grid_visible(7.99));
  CHECK(sample_grid_visible(8.0));
  CHECK(snap_device(2.5, 1.0) == 3.0);
  CHECK(snap_device(2.5, 0.99) == 2.5);
}

TEST_CASE("render_test_view_transform: the pan clamp keeps 64 device pixels of the picture visible", "[fast][render]") {
  using ovt::render::detail::clamp_view;
  using ovt::render::detail::ImageArea;
  const ImageArea area{0, 0, 1920, 1080};
  const ViewportSize vp{800, 600};
  // Zoom 1: the right edge may reach 64 px from the left of the viewport: pan <= 1920 - (64 - 400) = 2256.
  CHECK(clamp_view(view_of(1.0, 5000, 540), vp, area).pan_x == 2256.0);
  CHECK(clamp_view(view_of(1.0, -5000, 540), vp, area).pan_x == -336.0);
  CHECK(clamp_view(view_of(1.0, 960, 540), vp, area).pan_x == 960.0);
  // A picture smaller than 64 device pixels: all of it stays visible; one that fits the viewport centres.
  const ImageArea tiny{0, 0, 10, 10};
  CHECK(clamp_view(view_of(2.0, 500, 500), vp, tiny).pan_x <= 10.0 + 400.0 / 2.0);
  // A zero-size viewport leaves the pan alone.
  CHECK(clamp_view(view_of(1.0, 5000, 5000), ViewportSize{0, 0}, area).pan_x == 5000.0);
}

TEST_CASE("render_test_view_transform: sample grid lines and raster (R-ZOOM-4)", "[fast][render]") {
  using ovt::render::detail::rasterize_sample_grid;
  using ovt::render::detail::sample_grid_lines;
  // 4 x 3 samples at zoom 8 on a 40 x 30 viewport, picture offset 4 px from the left and top.
  const ViewState v = view_of(8.0, 2.0, 1.5);
  const ViewportSize vp{40, 30};
  const pl_rect2df source{0, 0, 4, 3};
  const pl_rect2df picture{4, 3, 36, 27};
  const auto lines = sample_grid_lines(v, vp, source, picture);
  REQUIRE(lines.columns.size() == 5);  // sample edges 0..3 and the right picture edge
  CHECK(lines.columns.front() == 4);
  CHECK(lines.columns.at(1) == 12);
  CHECK(lines.columns.back() == 35);
  REQUIRE(lines.rows.size() == 4);
  CHECK(lines.rows.front() == 3);
  CHECK(lines.rows.back() == 26);
  const auto px = rasterize_sample_grid(lines, vp, picture);
  REQUIRE(px.size() == static_cast<size_t>(40 * 30 * 4));
  auto alpha = [&px](int x, int y) { return px.at((static_cast<size_t>((y * 40) + x) * 4) + 3); };
  auto red = [&px](int x, int y) { return px.at(static_cast<size_t>((y * 40) + x) * 4); };
  CHECK(alpha(4, 10) == 0x66);  // on a column line
  CHECK(red(4, 10) == 51);      // #808080 x 0.4, premultiplied
  CHECK(alpha(8, 3) == 0x66);   // on a row line
  CHECK(alpha(5, 10) == 0);     // inside a sample
  CHECK(alpha(3, 10) == 0);     // outside the picture
  CHECK(alpha(12, 3) == 0x66);  // a crossing is written once, not blended twice
  // Below zoom 8 nothing is drawn.
  CHECK(sample_grid_lines(view_of(7.9, 2.0, 1.5), vp, source, picture).columns.empty());
}

TEST_CASE("render_test_view_transform: the sample value under the cursor is the plane sample", "[fast][render]") {
  using ovt::render::detail::sample_value;
  OwnedPicture op(64, 32, 8, 1, 1);
  for (uint32_t y = 0; y < 32; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      op.set(0, x, y, (x * 3 + y * 5) & 0xFF);
    }
  }
  for (uint32_t y = 0; y < 16; ++y) {
    for (uint32_t x = 0; x < 32; ++x) {
      op.set(1, x, y, 100 + x + y);
    }
  }
  CHECK(sample_value(op.pic, 0, 10, 20).value_or(9999) == ((10U * 3 + 20U * 5) & 0xFFU));
  CHECK(sample_value(op.pic, 1, 10, 20).value_or(9999) == 100U + 5U + 10U);  // chroma sample (5, 10) covers it
  CHECK_FALSE(sample_value(op.pic, 0, 64, 0).has_value());
  CHECK_FALSE(sample_value(op.pic, 0, -1, 0).has_value());
  CHECK_FALSE(sample_value(op.pic, 3, 0, 0).has_value());
  OwnedPicture deep(16, 8, 10, 3, 2);
  deep.set(0, 3, 4, 0x2A5);
  CHECK(sample_value(deep.pic, 0, 3, 4).value_or(0) == 0x2A5U);
}

// Acceptance test 2: zoom 8 on an 8-bit frame draws the 1-px grid and matches grid_zoom8.png; the value under the
// cursor over sample (10, 20) is the plane sample.
TEST_CASE("render_test_view_transform: the sample grid at zoom 8 matches grid_zoom8.png (AT2)",
          "[fast][render][vulkan]") {
  constexpr uint32_t kW = 16;
  constexpr uint32_t kH = 12;
  constexpr int kZoom = 8;
  auto surface = ovt::render::test::surface_or_skip(kW * kZoom, kH * kZoom);
  OwnedPicture op(kW, kH, 8, 1, 1);  // 4:2:0 with neutral chroma: a grey picture
  op.fill_plane(1, 128);
  op.fill_plane(2, 128);
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      op.set(0, x, y, ((x + y) & 1U) != 0 ? 235U : 16U);  // checkerboard: full-range black and white
    }
  }
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  ViewState view = view_of(kZoom, kW / 2.0, kH / 2.0);
  view.dither = false;
  surface->render(view);
  REQUIRE(std::string(surface->last_error()).empty());
  OutputPixels with_grid;
  REQUIRE(ovt::render::detail::download_output(*surface, with_grid));

  auto px = [&with_grid](int x, int y) {
    const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(with_grid.width)) + static_cast<size_t>(x)) * 4;
    return with_grid.bytes.at(i);
  };
  // Sample (3, 2) is white and fills device [24, 32) x [16, 24): its left column and top row are grid pixels (#808080
  // at alpha 0x66 over white darkens them), the rest is the sample, equal over its whole 7 x 7 interior.
  const int interior = px(26, 18);
  INFO("sample (3, 2): interior " << interior << " left " << px(24, 18) << " top " << px(26, 16));
  CHECK(interior >= 250);
  CHECK(px(24, 18) <= interior - 20);
  CHECK(px(26, 16) <= interior - 20);
  for (int y = 17; y < 24; ++y) {
    for (int x = 25; x < 32; ++x) {
      REQUIRE(px(x, y) == interior);
    }
  }
  // Sample (2, 2) is black: the grid lightens its edge pixels.
  CHECK(px(18, 18) <= 3);
  CHECK(px(16, 18) >= 20);

  RgbImage img;
  img.width = with_grid.width;
  img.height = with_grid.height;
  for (size_t i = 0; i + 3 < with_grid.bytes.size(); i += 4) {
    img.rgb.insert(img.rgb.end(), with_grid.bytes.begin() + static_cast<std::ptrdiff_t>(i),
                   with_grid.bytes.begin() + static_cast<std::ptrdiff_t>(i + 3));
  }
  check_golden("grid_zoom8.png", img);

  // The value under the cursor over sample (10, 20) of a larger frame equals the plane sample.
  OwnedPicture big(64, 32, 8, 1, 1);
  for (uint32_t y = 0; y < 32; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      big.set(0, x, y, (x * 3 + y * 5) & 0xFF);
    }
  }
  const ViewState cursor_view = view_of(kZoom, 10.5, 20.5);
  const ViewportSize vp{kW * kZoom, kH * kZoom};
  const auto s = sample_at_device(cursor_view, vp, vp.w / 2 + 3, vp.h / 2 + 3);
  REQUIRE(s.x == 10);
  REQUIRE(s.y == 20);
  CHECK(ovt::render::detail::sample_value(big.pic, 0, s.x, s.y).value_or(9999) == ((10U * 3 + 20U * 5) & 0xFFU));
}
