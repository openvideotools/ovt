// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.436 (chapter 10 §10.11): deblocking edges (pass 7), reference-index fill (pass 3, rows 22-29) and the
// selection outline, probe crosshair and selection rectangle (passes 10 and 12) of §10.5.6, with the goldens
// deblock_edges.png, refidx_fill.png and selection.png.
//
//   AT1: a selected block's outline pixels are #FFFFFF with a #000000 1-px halo (golden selection.png).
//   AT2: at zoom 1 (cell 4 px) no deblocking edges are drawn.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fixtures/make_synthetic.h"
#include "overlay/overlay_set_impl.h"
#include "overlay/pass_misc.h"
#include "ovt/render/overlay_colors.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"
#include "png_io.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::OverlayBuilder;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::OverlaySet;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::IntraCodec;
using ovt::render::detail::MarkRect;
using ovt::render::detail::MarkView;
using ovt::render::detail::OutputPixels;
using ovt::render::test::OwnedPicture;
using ovt::render::test::RgbImage;
using ovt::render::test::surface_or_skip;

// The fixture is 128 x 96 luma samples; the goldens draw it at zoom 4 (a cell is 16 device px).
constexpr int kPicW = 128;
constexpr int kPicH = 96;
constexpr int kZoom = 4;
constexpr int kW = kPicW * kZoom;
constexpr int kH = kPicH * kZoom;

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

std::unique_ptr<OverlaySet> build_misc(const OverlayConfig& cfg) {
  const std::vector<uint8_t> fa = ovt::render::test::make_misc_fixture();
  auto surface = VideoSurface::create("misc-test");  // the CPU build never touches the (uninitialised) surface
  REQUIRE(surface != nullptr);
  OverlayBuilder builder(*surface);
  auto set = builder.build(fa.data(), fa.size(), cfg);
  REQUIRE(set != nullptr);
  return set;
}

OwnedPicture grey_picture() {
  OwnedPicture op(kPicW, kPicH, 8, 1, 1);
  op.fill_plane(0, 126);
  op.fill_plane(1, 128);
  op.fill_plane(2, 128);
  op.pic.generation = 1;
  return op;
}

ViewState view_at(double zoom) {
  ViewState view;
  view.zoom = zoom;
  view.pan_x = kPicW / 2.0;
  view.pan_y = kPicH / 2.0;
  view.dither = false;
  return view;
}

OutputPixels render_with(VideoSurface& surface, const OverlayConfig* cfg, const ViewState& view) {
  if (cfg != nullptr) {
    const std::vector<uint8_t> fa = ovt::render::test::make_misc_fixture();
    OverlayBuilder builder(surface);
    auto set = builder.build(fa.data(), fa.size(), *cfg);
    REQUIRE(set != nullptr);
    surface.set_overlays(std::move(set));
  } else {
    surface.set_overlays(nullptr);
  }
  REQUIRE(std::string(surface.last_error()).empty());
  surface.render(view);
  REQUIRE(std::string(surface.last_error()).empty());
  OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(surface, px));
  return px;
}

std::array<int, 3> rgb_at(const OutputPixels& px, int x, int y) {
  const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(px.width)) + static_cast<size_t>(x)) * 4;
  return {px.bytes.at(i), px.bytes.at(i + 1), px.bytes.at(i + 2)};
}

std::array<int, 3> rgb_of(uint32_t rgba) {
  return {static_cast<int>(rgba >> 24U), static_cast<int>((rgba >> 16U) & 0xFFU),
          static_cast<int>((rgba >> 8U) & 0xFFU)};
}

RgbImage to_rgb(const OutputPixels& px) {
  RgbImage img;
  img.width = px.width;
  img.height = px.height;
  img.rgb.reserve(static_cast<size_t>(px.width) * static_cast<size_t>(px.height) * 3);
  for (size_t i = 0; i + 3 < px.bytes.size(); i += 4) {
    img.rgb.insert(img.rgb.end(), px.bytes.begin() + static_cast<std::ptrdiff_t>(i),
                   px.bytes.begin() + static_cast<std::ptrdiff_t>(i + 3));
  }
  return img;
}

// The golden rule of the other overlay tests (SPEC-ASSUMPTION(#469)): a pixel differs when a channel is more than 2
// codes off; at most 0.1 % may differ. OVT_UPDATE_GOLDEN=1 rewrites the golden (never on CI).
void check_golden(const RgbImage& actual, const std::string& name) {
  const std::string golden = std::string(OVT_RENDER_GOLDEN_DIR) + "/" + name;
  const char* update = std::getenv("OVT_UPDATE_GOLDEN");  // NOLINT(concurrency-mt-unsafe): test setup
  const char* ci = std::getenv("CI");                     // NOLINT(concurrency-mt-unsafe)
  if (update != nullptr && std::string(update) == "1" && (ci == nullptr || std::string(ci).empty())) {
    REQUIRE(ovt::render::test::write_png(golden, actual, true));
    WARN("rewrote golden " << golden);
    return;
  }
  const std::optional<RgbImage> read = ovt::render::test::read_png(golden);
  REQUIRE(read.has_value());
  const RgbImage expected = read.value_or(RgbImage{});
  REQUIRE(expected.width == actual.width);
  REQUIRE(expected.height == actual.height);
  size_t differing = 0;
  for (size_t p = 0; p < actual.rgb.size(); p += 3) {
    for (size_t c = 0; c < 3; ++c) {
      if (std::abs(int{actual.rgb.at(p + c)} - int{expected.rgb.at(p + c)}) > 2) {
        ++differing;
        break;
      }
    }
  }
  const size_t pixels = actual.rgb.size() / 3;
  if (differing * 1000 > pixels) {
    const std::string out = name.substr(0, name.size() - 4) + ".actual.png";
    static_cast<void>(ovt::render::test::write_png(out, actual, true));
    FAIL(differing << " of " << pixels << " pixels differ from " << golden << "; this render: " << out);
  }
}

MarkView mark_view(float zoom, float dpr) {
  MarkView v;
  v.viewport_half = {static_cast<float>(kPicW) * zoom / 2.0F, static_cast<float>(kPicH) * zoom / 2.0F};
  v.pan = {kPicW / 2.0F, kPicH / 2.0F};
  v.clip = {0.0F, 0.0F, static_cast<float>(kPicW) * zoom, static_cast<float>(kPicH) * zoom};
  v.zoom = zoom;
  v.dpr = dpr;
  return v;
}

bool covers(const std::vector<MarkRect>& rects, float x, float y, uint32_t rgba) {
  return std::ranges::any_of(
      rects, [&](const MarkRect& r) { return r.rgba == rgba && x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; });
}

}  // namespace

TEST_CASE("render_test_overlay_misc deblock edge classes (§10.5.1, §10.5.2 rows 13-14)", "[fast][render][overlay]") {
  using ovt::render::detail::deblock_edge_class;
  for (const IntraCodec c : {IntraCodec::H264, IntraCodec::H265, IntraCodec::AV1, IntraCodec::Unknown}) {
    CHECK(deblock_edge_class(c, 0) == 0);
    CHECK(deblock_edge_class(c, 0xFF) == 0);  // unknown
  }
  CHECK(deblock_edge_class(IntraCodec::H264, 1) == 1);
  CHECK(deblock_edge_class(IntraCodec::H264, 2) == 1);
  CHECK(deblock_edge_class(IntraCodec::H264, 3) == 2);
  CHECK(deblock_edge_class(IntraCodec::H264, 4) == 2);
  CHECK(deblock_edge_class(IntraCodec::H265, 1) == 1);
  CHECK(deblock_edge_class(IntraCodec::H265, 2) == 2);
  CHECK(deblock_edge_class(IntraCodec::AV1, 4) == 1);
  CHECK(deblock_edge_class(IntraCodec::AV1, 8) == 1);
  CHECK(deblock_edge_class(IntraCodec::AV1, 16) == 2);
  CHECK(deblock_edge_class(IntraCodec::Unknown, 2) == 2);
}

TEST_CASE("render_test_overlay_misc builder: edge texels and selection rects", "[fast][render][overlay]") {
  OverlayConfig cfg;
  SECTION("selection rects are built whatever the layers; deblock texels only with DeblockEdges") {
    const auto set = build_misc(cfg);
    CHECK(set->deblock_edges.empty());
    REQUIRE(set->block_rects.size() == 9);
    CHECK(set->block_rects[8] == std::array<int32_t, 4>{32, 32, 64, 64});
    REQUIRE(set->mv_rects.size() == 3);
    CHECK(set->mv_rects[1] == std::array<int32_t, 4>{64, 32, 80, 48});
  }
  SECTION("DeblockEdges: R = left edge, G = top edge, classified for HEVC") {
    cfg.layers = bit(OverlayLayer::DeblockEdges);
    const auto set = build_misc(cfg);
    REQUIRE(set->deblock_edges.size() == size_t{32} * 24 * 2);
    const auto at = [&](uint32_t cx, uint32_t cy, uint32_t c) {
      return set->deblock_edges[(((cy * 32) + cx) * 2) + c];
    };
    CHECK(at(8, 0, 0) == 2);   // bS 2
    CHECK(at(2, 0, 0) == 1);   // bS 1
    CHECK(at(3, 0, 0) == 0);   // not on the 8x8 grid
    CHECK(at(8, 9, 0) == 0);   // the gap rows
    CHECK(at(5, 16, 1) == 2);  // top edge, bS 2
    CHECK(at(5, 2, 1) == 1);
  }
}

TEST_CASE("render_test_overlay_misc marks geometry (§10.5.6 passes 10, 12)", "[fast][render][overlay]") {
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::Selection) | bit(OverlayLayer::Probe) | bit(OverlayLayer::SelectionRect);
  const auto set = build_misc(cfg);
  std::vector<MarkRect> rects;
  const uint32_t white = ovt::render::kOverlayColors.at(15);
  const uint32_t black = ovt::render::kOverlayHalo.at(15);

  SECTION("nothing is drawn for 'none' values or layers that are off") {
    ovt::render::detail::build_marks(*set, cfg, mark_view(4.0F, 1.0F), rects);
    CHECK(rects.empty());
    OverlayConfig off;
    off.selected_block = 8;
    off.probe_x = 3;
    off.probe_y = 3;
    off.selection_rect = ovt::fb::Rect(0, 0, 8, 8);
    ovt::render::detail::build_marks(*set, off, mark_view(4.0F, 1.0F), rects);
    CHECK(rects.empty());
    cfg.selected_block = 9;  // past the block list
    cfg.selected_mv = 3;
    ovt::render::detail::build_marks(*set, cfg, mark_view(4.0F, 1.0F), rects);
    CHECK(rects.empty());
  }
  SECTION("the selection outline is 2 device px with a 1-px halo at every zoom") {
    cfg.selected_block = 8;  // (32, 32)-(64, 64)
    for (const float zoom : {1.0F, 4.0F, 16.0F}) {
      ovt::render::detail::build_marks(*set, cfg, mark_view(zoom, 1.0F), rects);
      const float edge = (32.0F - 64.0F) * zoom + (64.0F * zoom);  // left edge x, device px
      const float mid = (48.0F - 48.0F) * zoom + (48.0F * zoom);   // a row through the block
      INFO("zoom " << zoom);
      CHECK(covers(rects, edge - 1.0F, mid, white));
      CHECK(covers(rects, edge, mid, white));
      CHECK(covers(rects, edge - 2.0F, mid, black));
      CHECK_FALSE(covers(rects, edge - 3.0F, mid, black));
      CHECK_FALSE(covers(rects, edge + 1.0F, mid, white));
    }
  }
  SECTION("a device pixel ratio of 2 doubles the widths") {
    cfg.selected_mv = 1;  // (64, 32)-(80, 48)
    ovt::render::detail::build_marks(*set, cfg, mark_view(4.0F, 2.0F), rects);
    const float left = 256.0F;
    CHECK(covers(rects, left - 2.0F, 160.0F, white));
    CHECK(covers(rects, left + 1.0F, 160.0F, white));
    CHECK(covers(rects, left - 4.0F, 160.0F, black));
    CHECK_FALSE(covers(rects, left - 5.0F, 160.0F, black));
  }
  SECTION("probe crosshair through the sample centre, across the picture") {
    cfg.probe_x = 10;
    cfg.probe_y = 20;
    ovt::render::detail::build_marks(*set, cfg, mark_view(4.0F, 1.0F), rects);
    const uint32_t red = ovt::render::kOverlayColors.at(17);
    CHECK(covers(rects, 42.0F, 0.0F, red));    // vertical line at floor((10.5) * 4) = 42
    CHECK(covers(rects, 511.0F, 82.0F, red));  // horizontal line at 82, up to the right edge
    CHECK(covers(rects, 41.0F, 0.0F, ovt::render::kOverlayHalo.at(17)));
    CHECK(covers(rects, 43.0F, 0.0F, ovt::render::kOverlayHalo.at(17)));
  }
  SECTION("selection rectangle: translucent fill inside a 1-px border, halo outside") {
    cfg.selection_rect = ovt::fb::Rect(8, 8, 16, 12);  // device (32, 32)-(96, 80)
    ovt::render::detail::build_marks(*set, cfg, mark_view(4.0F, 1.0F), rects);
    const uint32_t cyan = ovt::render::kOverlayColors.at(18);
    CHECK(covers(rects, 32.0F, 50.0F, cyan));
    CHECK(covers(rects, 95.0F, 50.0F, cyan));
    CHECK(covers(rects, 50.0F, 50.0F, ovt::render::overlay_fill_rgba(18, false)));
    CHECK(covers(rects, 31.0F, 50.0F, ovt::render::kOverlayHalo.at(18)));
    CHECK_FALSE(covers(rects, 96.0F + 1.0F, 50.0F, ovt::render::kOverlayHalo.at(18)));
  }
}

TEST_CASE("render_test_overlay_misc deblock_edges golden and AT2 (§10.5.1)", "[fast][render][vulkan][overlay]") {
  const OwnedPicture pic = grey_picture();
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::DeblockEdges);

  SECTION("deblock_edges.png at zoom 4") {
    auto surface = surface_or_skip(kW, kH);
    surface->set_frame(pic.pic);
    const OutputPixels plain = render_with(*surface, nullptr, view_at(kZoom));
    const OutputPixels first = render_with(*surface, &cfg, view_at(kZoom));
    CHECK(render_with(*surface, &cfg, view_at(kZoom)).bytes == first.bytes);
    check_golden(to_rgb(first), "deblock_edges.png");
    // The strong edge at x = 32 (device 128): a 1-px row-14 band, no halo; the weak one at x = 8 (device 32) has the
    // black halo of row 13 on both sides.
    CHECK(rgb_at(first, 128, 8) == rgb_of(ovt::render::kOverlayColors.at(14)));
    CHECK(rgb_at(first, 127, 8) == rgb_at(plain, 127, 8));
    CHECK(rgb_at(first, 129, 8) == rgb_at(plain, 129, 8));
    CHECK(rgb_at(first, 32, 8) != rgb_at(plain, 32, 8));
    CHECK(rgb_at(first, 31, 8) == std::array<int, 3>{0, 0, 0});
    CHECK(rgb_at(first, 33, 8) == std::array<int, 3>{0, 0, 0});
    CHECK(rgb_at(first, 128, 168) == rgb_at(plain, 128, 168));  // rows 8..11 have no vertical edges
    CHECK(rgb_at(first, 2, 2) == rgb_at(plain, 2, 2));          // the picture's left and top edges are not drawn
  }
  SECTION("AT2: at zoom 1 (cell 4 px) no deblocking edges are drawn; at zoom 2 (8 px) they are") {
    auto surface = surface_or_skip(kPicW * 2, kPicH * 2);
    surface->set_frame(pic.pic);
    const ViewState one = view_at(1.0);
    const OutputPixels plain1 = render_with(*surface, nullptr, one);
    CHECK(render_with(*surface, &cfg, one).bytes == plain1.bytes);
    const OutputPixels plain2 = render_with(*surface, nullptr, view_at(2.0));
    CHECK(render_with(*surface, &cfg, view_at(2.0)).bytes != plain2.bytes);
  }
}

TEST_CASE("render_test_overlay_misc refidx_fill golden (§10.5.2 rows 22-29)", "[fast][render][vulkan][overlay]") {
  const OwnedPicture pic = grey_picture();
  auto surface = surface_or_skip(kW, kH);
  surface->set_frame(pic.pic);
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::RefIdxFill);
  const OutputPixels plain = render_with(*surface, nullptr, view_at(kZoom));
  const OutputPixels first = render_with(*surface, &cfg, view_at(kZoom));
  check_golden(to_rgb(first), "refidx_fill.png");
  // Eight slots: ref0 = cx / 4 + 2 * (cy / 4), so the cells (0 or 4, 0 / 4 / 8 / 12) hold slots 0..7; all differ.
  std::vector<std::array<int, 3>> seen;
  for (int slot = 0; slot < 8; ++slot) {
    const int sx = ((slot % 2) * 4 * 4 * kZoom) + 8;
    const int sy = ((slot / 2) * 4 * 4 * kZoom) + 8;
    const auto c = rgb_at(first, sx, sy);
    CHECK(c != rgb_at(plain, sx, sy));
    for (const auto& s : seen) {
      CHECK(s != c);
    }
    seen.push_back(c);
  }
  CHECK(rgb_at(first, 8, (21 * 4 * kZoom) + 8) == seen.front());  // cy 21, cx 0: ref1 = 0 -> slot 0
}

TEST_CASE("render_test_overlay_misc selection golden and AT1 (§10.5.2 rows 15, 17, 18)",
          "[fast][render][vulkan][overlay]") {
  const OwnedPicture pic = grey_picture();
  auto surface = surface_or_skip(kW, kH);
  surface->set_frame(pic.pic);
  const OutputPixels plain = render_with(*surface, nullptr, view_at(kZoom));
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::Selection) | bit(OverlayLayer::Probe) | bit(OverlayLayer::SelectionRect);
  cfg.selected_block = 8;  // CU (32, 32)-(64, 64) -> device (128, 128)-(256, 256)
  cfg.probe_x = 100;
  cfg.probe_y = 10;
  cfg.selection_rect = ovt::fb::Rect(72, 60, 40, 24);
  const OutputPixels first = render_with(*surface, &cfg, view_at(kZoom));
  CHECK(render_with(*surface, &cfg, view_at(kZoom)).bytes == first.bytes);
  check_golden(to_rgb(first), "selection.png");

  // AT1: the outline is #FFFFFF, 2 px straddling the block edge, with a #000000 1-px halo outside.
  const std::array<int, 3> white{255, 255, 255};
  const std::array<int, 3> black{0, 0, 0};
  CHECK(rgb_at(first, 127, 192) == white);
  CHECK(rgb_at(first, 128, 192) == white);
  CHECK(rgb_at(first, 126, 192) == black);
  CHECK(rgb_at(first, 125, 192) == rgb_at(plain, 125, 192));
  CHECK(rgb_at(first, 129, 192) == rgb_at(plain, 129, 192));
  CHECK(rgb_at(first, 192, 255) == white);
  CHECK(rgb_at(first, 192, 256) == white);
  CHECK(rgb_at(first, 192, 257) == black);
  CHECK(rgb_at(first, 192, 258) == rgb_at(plain, 192, 258));
  // The probe crosshair (row 17) at device x = floor(100.5 * 4) = 402, y = 42.
  CHECK(rgb_at(first, 402, 370) == rgb_of(ovt::render::kOverlayColors.at(17)));
  CHECK(rgb_at(first, 20, 42) == rgb_of(ovt::render::kOverlayColors.at(17)));
  // The selection rectangle border (row 18) at device x = 288.
  CHECK(rgb_at(first, 288, 300) == rgb_of(ovt::render::kOverlayColors.at(18)));
}
