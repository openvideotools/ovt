// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.435 (chapter 10 §10.11): id buffer, asynchronous readback and VideoSurface::pick() (§10.5.6, §10.5.7), and the
// probe's CPU colour math (§10.6.3, render/src/color/cpu_convert.cpp).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "color/cpu_convert.h"
#include "fixtures/make_synthetic.h"
#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"
#include "ovt_model_generated.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using Catch::Approx;
using ovt::render::OverlayBuilder;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::Pick;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::CodeColor;
using ovt::render::detail::ProbeLight;
using ovt::render::test::OwnedPicture;
using ovt::render::test::surface_or_skip;

constexpr uint64_t kWaitNs = 5'000'000'000ULL;

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

ViewState view_at(double zoom, double cx, double cy) {
  ViewState v;
  v.zoom = zoom;
  v.pan_x = cx;
  v.pan_y = cy;
  v.dither = false;
  return v;
}

OwnedPicture grey(uint32_t w, uint32_t h) {
  OwnedPicture op(w, h, 8, 1, 1);
  op.fill_plane(0, 126);
  op.fill_plane(1, 128);
  op.fill_plane(2, 128);
  op.pic.generation = 1;
  return op;
}

void set_overlays(VideoSurface& surface, const std::vector<uint8_t>& fa, const OverlayConfig& cfg) {
  OverlayBuilder builder(surface);
  auto set = builder.build(fa.data(), fa.size(), cfg);
  REQUIRE(set != nullptr);
  surface.set_overlays(std::move(set));
  REQUIRE(std::string(surface.last_error()).empty());
}

// Renders and waits for the id readback, so the next pick() sees this frame.
void render_and_wait(VideoSurface& surface, const ViewState& view) {
  surface.render(view);
  REQUIRE(std::string(surface.last_error()).empty());
  REQUIRE(ovt::render::detail::wait_id_readback(surface, kWaitNs));
}

// make_synthetic_mv's MotionVector order: 64x64 superblocks in raster order, 4x4 cells in raster order inside each.
int32_t synthetic_mv_index(uint32_t width, uint32_t x4, uint32_t y4) {
  const uint32_t cw = (width + 3) / 4;
  const uint32_t sb_cols = (width + 63) / 64;
  int32_t index = 0;
  const uint32_t sx = x4 / 16;
  const uint32_t sy = y4 / 16;
  for (uint32_t s = 0; s < (sy * sb_cols) + sx; ++s) {
    const uint32_t col = s % sb_cols;
    const uint32_t cols_in_sb = std::min(cw, (col * 16) + 16) - (col * 16);
    index += static_cast<int32_t>(cols_in_sb * 16);  // rows of a superblock above the last are always full here
  }
  const uint32_t cols_here = std::min(cw, (sx * 16) + 16) - (sx * 16);
  return index + static_cast<int32_t>(((y4 - (sy * 16)) * cols_here) + (x4 - (sx * 16)));
}

}  // namespace

// ---- CPU colour math (step 5) -----------------------------------------------------------------------------------

TEST_CASE("render_test_pick_pq_curve", "[fast][render][pick]") {
  using ovt::render::detail::pq_eotf_nits;
  using ovt::render::detail::pq_oetf;
  CHECK(pq_eotf_nits(0.0F) == Approx(0.0F).margin(1e-6));
  CHECK(pq_eotf_nits(1.0F) == Approx(10000.0F).epsilon(1e-5));
  CHECK(pq_eotf_nits(0.5807F) == Approx(203.0F).epsilon(0.002));  // BT.2408 reference white
  for (const float nits : {0.1F, 1.0F, 100.0F, 203.0F, 1000.0F, 4000.0F}) {
    CHECK(pq_eotf_nits(pq_oetf(nits)) == Approx(nits).epsilon(1e-4));
  }
}

TEST_CASE("render_test_pick_sdr_white_and_chromaticity", "[fast][render][pick]") {
  const CodeColor c;  // BT.709 limited 8-bit, BT.1886
  ProbeLight l;
  REQUIRE(ovt::render::detail::code_to_light(c, {235, 128, 128}, l));
  CHECK(l.nits_maxrgb == Approx(203.0F).epsilon(1e-4));
  CHECK(l.nits_luminance == Approx(203.0F).epsilon(1e-3));
  CHECK(l.cie_x == Approx(0.3127F).margin(5e-4));  // D65
  CHECK(l.cie_y == Approx(0.3290F).margin(5e-4));
  // Pure red: BT.709 red primary chromaticity.
  std::array<float, 3> rgb{};
  REQUIRE(ovt::render::detail::code_to_nonlinear_rgb(c, {63, 102, 240}, rgb));
  CHECK(rgb[0] == Approx(1.0F).margin(0.01));
  CHECK(rgb[1] == Approx(0.0F).margin(0.01));
  CHECK(rgb[2] == Approx(0.0F).margin(0.01));
  // Full-range 10-bit RGB (planes G, B, R) and 4:0:0 neutral chroma.
  CodeColor rgb10;
  rgb10.sys = PL_COLOR_SYSTEM_RGB;
  rgb10.levels = PL_COLOR_LEVELS_FULL;
  rgb10.bit_depth = 10;
  REQUIRE(ovt::render::detail::code_to_nonlinear_rgb(rgb10, {1023, 0, 0}, rgb));
  CHECK(rgb == std::array<float, 3>{0.0F, 1.0F, 0.0F});
  REQUIRE(ovt::render::detail::code_to_nonlinear_rgb(c, {126, -1, -1}, rgb));
  CHECK(rgb[0] == Approx(rgb[1]));
  CHECK(rgb[1] == Approx(rgb[2]));
}

TEST_CASE("render_test_pick_hlg_reference_white", "[fast][render][pick]") {
  // BT.2408: HLG 75 % signal is reference white (203 cd/m²) on a 1000 cd/m² display with system gamma 1.2.
  CodeColor c;
  c.sys = PL_COLOR_SYSTEM_BT_2020_NC;
  c.primaries = PL_COLOR_PRIM_BT_2020;
  c.transfer = PL_COLOR_TRC_HLG;
  c.levels = PL_COLOR_LEVELS_FULL;
  c.bit_depth = 10;
  ProbeLight l;
  REQUIRE(ovt::render::detail::code_to_light(c, {767, 512, 512}, l));  // 767 / 1023 = 0.75
  CHECK(l.nits_maxrgb == Approx(203.0F).epsilon(0.01));
  CHECK(l.rgb_linear[0] == Approx(1.0F).epsilon(0.01));
  // Unsupported systems report "unavailable".
  c.sys = PL_COLOR_SYSTEM_BT_2100_PQ;
  CHECK_FALSE(ovt::render::detail::code_to_light(c, {767, 512, 512}, l));
  CHECK(l.nits_maxrgb == -1.0F);
}

// ---- pick() on the GPU ------------------------------------------------------------------------------------------

TEST_CASE("render_test_pick_mv_index_at_arrow_tip (AT1)", "[fast][vulkan][render][pick]") {
  // §10.9.2 synthetic MV fixture (rotation field); near the centre the arrows are shorter than a cell, so at zoom 8 a
  // tip is covered by its own arrow only.
  constexpr uint32_t kW = 3840;
  constexpr uint32_t kH = 2160;
  constexpr int kView = 512;  // 64 x 64 samples at zoom 8: the cells within 8 of the centre
  auto surface = surface_or_skip(kView, kView);
  const OwnedPicture pic = grey(kW, kH);
  surface->set_frame(pic.pic);
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::MvL0);
  set_overlays(*surface, ovt::render::test::make_synthetic_mv(kW, kH), cfg);
  const double zoom = 8.0;
  const double half = kView / 2.0;
  const double cx = kW / 2.0;
  const double cy = kH / 2.0;
  render_and_wait(*surface, view_at(zoom, cx, cy));

  const uint32_t cw = kW / 4;
  const uint32_t ch = kH / 4;
  int checked = 0;
  for (const auto& [dx, dy] : std::array<std::array<int, 2>, 4>{{{6, 0}, {0, 6}, {-5, 3}, {4, -6}}}) {
    const uint32_t x4 = (cw / 2) + static_cast<uint32_t>(dx);
    const uint32_t y4 = (ch / 2) + static_cast<uint32_t>(dy);
    // The generator's vector (1/16 samples) and the arrow pass's device mapping (§10.5.4).
    const double vx = std::clamp(-((static_cast<int>(y4) * 2) - static_cast<int>(ch)) * 2, -512, 512) / 16.0;
    const double vy = std::clamp(((static_cast<int>(x4) * 2) - static_cast<int>(cw)) * 2, -512, 512) / 16.0;
    const double len = std::hypot(vx, vy) * zoom;
    REQUIRE(len >= 8.0);  // a real arrow with a head, not a dot
    const double ox = ((x4 * 4.0 + 2.0 - cx) * zoom) + half;
    const double oy = ((y4 * 4.0 + 2.0 - cy) * zoom) + half;
    // Tip snapped to the pixel centre as in arrows.glsl; 2 px back along the shaft is inside the head triangle.
    const double tx = std::floor(ox + (vx * zoom)) + 0.5 - (2.0 * vx * zoom / len);
    const double ty = std::floor(oy + (vy * zoom)) + 0.5 - (2.0 * vy * zoom / len);
    const Pick p = surface->pick(static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)));
    INFO("cell " << x4 << "," << y4 << " tip pixel " << tx << "," << ty);
    CHECK(p.hit);
    CHECK(p.mv_index == synthetic_mv_index(kW, x4, y4));
    ++checked;
  }
  CHECK(checked == 4);
  // Between arrows (a cell corner far from every arrow of this short-vector region): no arrow.
  const Pick none = surface->pick(kView / 2 - 1, kView / 2 - 1);  // a cell corner, 16 px from the nearest origins
  CHECK(none.hit);
  CHECK(none.mv_index == -1);
  // Outside the picture.
  surface->render(view_at(1.0, -1000.0, -1000.0));
  CHECK_FALSE(surface->pick(10, 10).hit);
}

TEST_CASE("render_test_pick_pq_1000_nits (AT2)", "[fast][vulkan][render][pick]") {
  auto surface = surface_or_skip(64, 64);
  OwnedPicture pic(32, 32, 10, 1, 2);
  pic.pic.color.primaries = 9;  // BT.2020
  pic.pic.color.transfer = 16;  // PQ
  pic.pic.color.matrix = 9;     // BT.2020 NCL
  pic.fill_plane(0, 723);
  pic.fill_plane(1, 512);
  pic.fill_plane(2, 512);
  pic.pic.generation = 1;
  surface->set_frame(pic.pic);
  surface->render(view_at(1.0, 16.0, 16.0));
  REQUIRE(std::string(surface->last_error()).empty());
  const Pick p = surface->pick(32, 32);
  REQUIRE(p.hit);
  CHECK(p.image_x == 16);
  CHECK(p.image_y == 16);
  CHECK(p.code[0] == 723);
  CHECK(p.code[1] == 512);
  CHECK(p.code[2] == 512);
  CHECK(p.nits_maxrgb == Approx(1000.0F).epsilon(0.01));
  CHECK(p.nits_luminance == Approx(1000.0F).epsilon(0.01));
  CHECK(p.pq_code == Approx((723.0F - 64.0F) / 876.0F).margin(0.002));  // grey: PQ(max RGB) = Y'
  CHECK(p.mv_index == -1);
}

TEST_CASE("render_test_pick_block_id_and_codes", "[fast][vulkan][render][pick]") {
  // make_synthetic: block 0 for cx < cells_w / 2, block 1 elsewhere; the CPU lookup works with every layer hidden.
  auto surface = surface_or_skip(128, 128);
  const ovt::render::test::SyntheticOptions opts;
  const uint32_t w = opts.cells_w * 4;
  const uint32_t h = opts.cells_h * 4;
  OwnedPicture pic(w, h, 8, 1, 1);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      pic.set(0, x, y, (x + y) & 0xFFU);
    }
  }
  pic.fill_plane(1, 100);
  pic.fill_plane(2, 200);
  pic.pic.generation = 1;
  surface->set_frame(pic.pic);
  set_overlays(*surface, ovt::render::test::make_synthetic(opts), OverlayConfig{});
  render_and_wait(*surface, view_at(1.0, w / 2.0, h / 2.0));

  const auto image_at = [&](int px, int py) {
    return std::array<int, 2>{px - 64 + (static_cast<int>(w) / 2), py - 64 + (static_cast<int>(h) / 2)};
  };
  const Pick left = surface->pick(10, 20);
  const auto il = image_at(10, 20);
  REQUIRE(left.hit);
  CHECK(left.image_x == il[0]);
  CHECK(left.cell_x == il[0] / 4);
  CHECK(left.block_id == (static_cast<uint32_t>(il[0] / 4) < opts.cells_w / 2 ? 0U : 1U));
  CHECK(left.code[0] == ((il[0] + il[1]) & 0xFF));
  CHECK(left.code[1] == 100);
  CHECK(left.code[2] == 200);
  const Pick right = surface->pick(100, 20);
  CHECK(right.block_id == 1U);
  CHECK(right.mv_index == -1);  // the fill writes block ids, never the arrow bit

  // Sample windows: a move within 32 samples reuses the window, a larger one downloads a new one.
  const uint64_t before = ovt::render::detail::probe_window_downloads(*surface);
  const Pick near = surface->pick(102, 22);  // 6 samples from the last pick's window centre
  CHECK(ovt::render::detail::probe_window_downloads(*surface) == before);
  const auto in = image_at(102, 22);
  CHECK(near.code[0] == ((in[0] + in[1]) & 0xFF));
  const Pick far = surface->pick(10, 110);
  CHECK(ovt::render::detail::probe_window_downloads(*surface) == before + 1);
  const auto ifar = image_at(10, 110);
  CHECK(far.code[0] == ((ifar[0] + ifar[1]) & 0xFF));
}

TEST_CASE("render_test_pick_readback_lifecycle", "[fast][vulkan][render][pick]") {
  constexpr uint32_t kW = 256;
  constexpr uint32_t kH = 256;
  auto surface = surface_or_skip(256, 256);
  const OwnedPicture pic = grey(kW, kH);
  surface->set_frame(pic.pic);
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::MvL0);
  const std::vector<uint8_t> fa = ovt::render::test::make_synthetic_mv(kW, kH);
  set_overlays(*surface, fa, cfg);
  const ViewState view = view_at(1.0, kW / 2.0, kH / 2.0);

  // Several renders back to back without waiting: buffers are reused only after completion, and the result is the
  // newest completed frame.
  for (int i = 0; i < 6; ++i) {
    surface->render(view);
  }
  REQUIRE(ovt::render::detail::wait_id_readback(*surface, kWaitNs));
  // The arrow of cell (8, 8) points along (+, -) and its origin is (34, 34): the origin pixel is on its shaft.
  const Pick first = surface->pick(34, 34);
  CHECK(first.mv_index >= 0);

  // Arrows hidden: the next frame draws only block ids (none in this fixture), so the arrow is gone.
  cfg.layers = 0;
  set_overlays(*surface, fa, cfg);
  render_and_wait(*surface, view);
  CHECK(surface->pick(34, 34).mv_index == -1);

  // No overlays: the ids are forgotten.
  cfg.layers = bit(OverlayLayer::MvL0);
  set_overlays(*surface, fa, cfg);
  render_and_wait(*surface, view);
  CHECK(surface->pick(34, 34).mv_index == first.mv_index);
  surface->set_overlays(nullptr);
  surface->render(view);
  CHECK(surface->pick(34, 34).mv_index == -1);
}

TEST_CASE("render_test_pick_region_fallback", "[fast][vulkan][render][pick]") {
  // Step 6: 1x1 downloads at the last cursor; pick() reflects the previous frame's cursor.
  constexpr uint32_t kW = 256;
  constexpr uint32_t kH = 256;
  auto surface = surface_or_skip(256, 256);
  ovt::render::detail::set_id_region_mode(*surface, true);
  const OwnedPicture pic = grey(kW, kH);
  surface->set_frame(pic.pic);
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::MvL0);
  set_overlays(*surface, ovt::render::test::make_synthetic_mv(kW, kH), cfg);
  const ViewState view = view_at(1.0, kW / 2.0, kH / 2.0);
  render_and_wait(*surface, view);           // read at the initial cursor (0, 0)
  const Pick stale = surface->pick(34, 34);  // remembers (34, 34) for the next readback
  CHECK(stale.mv_index == -1);               // texel (0, 0) holds no arrow
  render_and_wait(*surface, view);           // the GUI's redundant pick after render()
  const Pick fresh = surface->pick(34, 34);
  CHECK(fresh.mv_index >= 0);
}
