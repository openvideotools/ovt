// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.454 (chapter 10 §10.11): luminance waveform, vectorscope and CIE xy scope textures (§10.7.6-§10.7.7), exposed
// through Measurements.waveform / vectorscope / cie_xy (§10.2.6) and ExportKind::Scope*Png (§10.8). The frames are
// flat 10-bit limited BT.2020 PQ pictures, so every sample of a scope lands on one point (or one row) whose position
// the CPU reference of render/src/measure/scopes.h predicts.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <libplacebo/colorspace.h>

#include "measure/cie1931_locus.h"
#include "measure/measure.h"
#include "measure/scopes.h"
#include "ovt/render/video_surface.h"
#include "render_test_support.h"

namespace {

using ovt::render::Measurements;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::test::OwnedPicture;
using ovt::render::test::surface_or_skip;
namespace detail = ovt::render::detail;
using detail::ScopeKind;

constexpr uint32_t kW = 1920;
constexpr uint32_t kH = 1080;

// 10-bit limited code of a [0, 1] signal value (luma: 64..940, chroma: 512 +- 448).
uint32_t luma_code(double e) {
  return static_cast<uint32_t>(std::lround(64.0 + (876.0 * e)));
}
uint32_t chroma_code(double c) {
  return static_cast<uint32_t>(std::lround(512.0 + (896.0 * c)));
}

// Flat 10-bit limited BT.2020 NCL PQ 4:2:0 picture of Y'CbCr codes (y, cb, cr).
OwnedPicture flat_pq(uint32_t y, uint32_t cb, uint32_t cr) {
  OwnedPicture op(kW, kH, 10, 1, 2);
  op.pic.color.primaries = 9;  // BT.2020
  op.pic.color.transfer = 16;  // PQ
  op.pic.color.matrix = 9;     // BT.2020 NCL
  op.fill_plane(0, y);
  op.fill_plane(1, cb);
  op.fill_plane(2, cr);
  return op;
}

// A neutral grey of `nits`.
OwnedPicture grey(double nits) {
  return flat_pq(luma_code(detail::pq_oetf(nits)), 512, 512);
}

// BT.2020 red at the full PQ signal: R' = 1, G' = B' = 0 (§10.7.7 colour-bar red).
OwnedPicture red2020() {
  const double y = 0.2627;
  return flat_pq(luma_code(y), chroma_code((0.0 - y) / 1.8814), chroma_code((1.0 - y) / 1.4746));
}

// The nits a limited luma code decodes to (neutral chroma).
double decoded_nits(uint32_t code) {
  return detail::pq_eotf_nits((static_cast<double>(code) - 64.0) / 876.0);
}

ViewState one_to_one(const OwnedPicture& op) {
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = op.pic.coded_width / 2.0;
  view.pan_y = op.pic.coded_height / 2.0;
  return view;
}

bool same_frame(const ovt::fb::FrameId& a, const ovt::fb::FrameId& b) {
  return a.decode_index() == b.decode_index() && a.display_index() == b.display_index();
}

// The consumer loop of §10.7.9 (as in test_measure.cpp): ask, render, poll until `op`'s result is back.
Measurements measure_frame(VideoSurface& surface, OwnedPicture& op, int index) {
  op.pic.id = ovt::fb::FrameId(index, index);
  op.pic.generation = static_cast<uint64_t>(index) + 1;
  surface.set_frame(op.pic);
  REQUIRE(std::string(surface.last_error()).empty());
  static_cast<void>(surface.measure());
  surface.render(one_to_one(op));
  REQUIRE(std::string(surface.last_error()).empty());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  Measurements m = surface.measure();
  while (!(m.valid && same_frame(m.measured_frame, op.pic.id)) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m = surface.measure();
  }
  REQUIRE(m.valid);
  REQUIRE(same_frame(m.measured_frame, op.pic.id));
  return m;
}

float half_to_float(uint16_t h) {
  const uint32_t exp = (h >> 10U) & 0x1FU;
  const uint32_t mant = h & 0x3FFU;
  const float v = exp == 0 ? std::ldexp(static_cast<float>(mant), -24)
                           : std::ldexp(static_cast<float>(mant | 0x400U), static_cast<int>(exp) - 25);
  return (h & 0x8000U) != 0 ? -v : v;
}

// A downloaded scope texture: channel c of texel (x, y) is log2(1 + count).
struct Scope {
  int width = 0;
  int height = 0;
  std::vector<uint16_t> texels;
  [[nodiscard]] float at(int x, int y, int c) const {
    const auto i = (static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x);
    return half_to_float(texels.at((i * 4) + static_cast<size_t>(c)));
  }
  // Approximate count (exact for small counts; RGBA16F keeps 11 significant bits of log2(1 + count)).
  [[nodiscard]] double count(int x, int y, int c) const { return std::exp2(static_cast<double>(at(x, y, c))) - 1.0; }
};

Scope download(VideoSurface& surface, ScopeKind kind) {
  Scope s;
  REQUIRE(detail::download_scope(surface, kind, s.texels, s.width, s.height));
  return s;
}

// The bounding box {x0, y0, x1, y1} of texels whose channel c is non-zero, or {-1, ...} when there is none.
std::array<int, 4> energy_box(const Scope& s, int c) {
  std::array<int, 4> box = {-1, -1, -1, -1};
  for (int y = 0; y < s.height; ++y) {
    for (int x = 0; x < s.width; ++x) {
      if (s.at(x, y, c) != 0.0F) {
        box = box[0] < 0 ? std::array<int, 4>{x, y, x, y}
                         : std::array<int, 4>{std::min(box[0], x), std::min(box[1], y), std::max(box[2], x),
                                              std::max(box[3], y)};
      }
    }
  }
  return box;
}

double total(const Scope& s, int c) {
  double sum = 0;
  for (int y = 0; y < s.height; ++y) {
    for (int x = 0; x < s.width; ++x) {
      sum += s.count(x, y, c);
    }
  }
  return sum;
}

}  // namespace

TEST_CASE("render_test_scopes: coordinate mappings (§10.7.6-§10.7.7)", "[fast][render]") {
  // Waveform column: floor(x / crop_w x 1024).
  CHECK(detail::waveform_column(0, 1920) == 0);
  CHECK(detail::waveform_column(1919, 1920) == 1023);
  CHECK(detail::waveform_column(960, 1920) == 512);
  CHECK(detail::waveform_column(5, 10) == 512);
  CHECK(detail::waveform_column(0, 0) == 0);

  // Log-nits rows: 0.0001 -> 0, 10 000 -> 511, one decade = 511 / 8 rows.
  using detail::WaveformAxis;
  CHECK(detail::waveform_row(1.0e-4, WaveformAxis::LogNits) == 0);
  CHECK(detail::waveform_row(0.0, WaveformAxis::LogNits) == 0);  // clamped
  CHECK(detail::waveform_row(1.0e4, WaveformAxis::LogNits) == 511);
  CHECK(detail::waveform_row(1.0e6, WaveformAxis::LogNits) == 511);
  CHECK(detail::waveform_row(1.0, WaveformAxis::LogNits) == 255);    // 511 x 4 / 8 = 255.5
  CHECK(detail::waveform_row(203.0, WaveformAxis::LogNits) == 402);  // 511 x 6.3075 / 8 = 402.9
  // PQ-code rows: 511 x PQ(nits).
  CHECK(detail::waveform_row(0.0, WaveformAxis::PqCode) == 0);
  CHECK(detail::waveform_row(1.0e4, WaveformAxis::PqCode) == 511);
  CHECK(detail::waveform_row(203.0, WaveformAxis::PqCode) == 296);  // PQ(203) = 0.5807
  CHECK(detail::waveform_texel_y(511) == 0);                        // brightest at the top
  CHECK(detail::waveform_texel_y(0) == 511);

  // Vectorscope: neutral -> centre; the colour-bar red of BT.2020 at (Cb, Cr) = (-0.1396, 0.5).
  const auto grey_cbcr = detail::vectorscope_cbcr({203.0, 203.0, 203.0});
  CHECK(grey_cbcr[0] == Catch::Approx(0.0).margin(1e-12));
  CHECK(grey_cbcr[1] == Catch::Approx(0.0).margin(1e-12));
  const auto red = detail::vectorscope_cbcr({10000.0, 0.0, 0.0});
  CHECK(red[0] == Catch::Approx(-0.2627 / 1.8814).margin(1e-5));
  CHECK(red[1] == Catch::Approx(0.5).margin(1e-5));  // PQ(0) = c1^m2 = 7e-7, not 0
  const auto uv1 = detail::vectorscope_uv(red[0], red[1], 1.0F);
  CHECK(uv1[0] == Catch::Approx(256.0 - (0.2627 / 1.8814 * 512.0)).margin(1e-2));
  CHECK(uv1[1] == Catch::Approx(0.0).margin(1e-3));
  const auto uv2 = detail::vectorscope_uv(0.1, -0.1, 2.0F);
  CHECK(uv2[0] == Catch::Approx(358.4));
  CHECK(uv2[1] == Catch::Approx(358.4));

  // CIE xy: x in [0, 0.8] over 512 columns, y in [0, 0.9] over 512 rows, top = 0.9.
  const auto d65 = detail::cie_uv(0.3127, 0.3290);
  CHECK(d65[0] == Catch::Approx(200.128));
  CHECK(d65[1] == Catch::Approx(324.8355).epsilon(1e-6));

  // Texels: within one texel of the edge -> clamped onto it, further out -> dropped.
  CHECK(detail::scope_texel(0.0, 0.0, 512) == std::array<int, 2>{0, 0});
  CHECK(detail::scope_texel(512.0, -0.5, 512) == std::array<int, 2>{511, 0});
  CHECK(detail::scope_texel(511.99, 300.5, 512) == std::array<int, 2>{511, 300});
  CHECK(detail::scope_texel(-1.5, 10.0, 512) == std::array<int, 2>{-1, -1});
  CHECK(detail::scope_texel(10.0, 513.5, 512) == std::array<int, 2>{-1, -1});
  CHECK(detail::scope_texel(std::nan(""), 10.0, 512) == std::array<int, 2>{-1, -1});
}

TEST_CASE("render_test_scopes: gamut point tables match libplacebo's matrices (§10.7.7)", "[fast][render]") {
  // Linear RGB of each gamut at 100 % (1.0 = 10 000 nits) -> BT.2020 -> (Cb, Cr): the precomputed tables.
  const std::array<std::array<double, 3>, 6> colours = {
      {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}}};
  const std::array<std::pair<detail::Gamut, enum pl_color_primaries>, 3> gamuts = {
      {{detail::Gamut::Bt2020, PL_COLOR_PRIM_BT_2020},
       {detail::Gamut::Bt709, PL_COLOR_PRIM_BT_709},
       {detail::Gamut::P3D65, PL_COLOR_PRIM_DISPLAY_P3}}};
  for (const auto& [gamut, prim] : gamuts) {
    const std::array<float, 9> m = detail::rgb_to_bt2020_rows(prim);
    const auto& table = detail::vectorscope_targets(gamut);
    for (size_t i = 0; i < colours.size(); ++i) {
      std::array<double, 3> rgb{};
      for (size_t r = 0; r < 3; ++r) {
        for (size_t c = 0; c < 3; ++c) {
          rgb.at(r) += static_cast<double>(m.at((r * 3) + c)) * colours.at(i).at(c) * 10000.0;
        }
      }
      const auto cbcr = detail::vectorscope_cbcr(rgb);
      INFO("gamut " << static_cast<int>(gamut) << " colour " << i);
      // 0.002 = 1 px at gain 1: libplacebo derives its matrices from the same xy primaries, rounded differently.
      CHECK(static_cast<double>(table.at(i).cb) == Catch::Approx(cbcr[0]).margin(0.002));
      CHECK(static_cast<double>(table.at(i).cr) == Catch::Approx(cbcr[1]).margin(0.002));
    }
  }
  // BT.2020 content keeps an exact identity, so its red stays on the hexagon point.
  CHECK(detail::rgb_to_bt2020_rows(PL_COLOR_PRIM_BT_2020) == std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1});
  // The hexagon is symmetric: complementary colours are opposite.
  const auto& hex = detail::vectorscope_targets(detail::Gamut::Bt2020);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(hex.at(i).cb == Catch::Approx(-hex.at(i + 3).cb).margin(1e-5));
    CHECK(hex.at(i).cr == Catch::Approx(-hex.at(i + 3).cr).margin(1e-5));
  }
  // CIE xy triangles: the primaries libplacebo uses.
  for (const auto& [gamut, prim] : gamuts) {
    const struct pl_raw_primaries* raw = pl_raw_primaries_get(prim);
    const auto& xy = detail::gamut_primaries_xy(gamut);
    CHECK(xy[0].x == Catch::Approx(raw->red.x).margin(1e-4));
    CHECK(xy[0].y == Catch::Approx(raw->red.y).margin(1e-4));
    CHECK(xy[1].x == Catch::Approx(raw->green.x).margin(1e-4));
    CHECK(xy[1].y == Catch::Approx(raw->green.y).margin(1e-4));
    CHECK(xy[2].x == Catch::Approx(raw->blue.x).margin(1e-4));
    CHECK(xy[2].y == Catch::Approx(raw->blue.y).margin(1e-4));
    CHECK(detail::d65_white_xy().x == Catch::Approx(raw->white.x).margin(1e-4));
    CHECK(detail::d65_white_xy().y == Catch::Approx(raw->white.y).margin(1e-4));
  }
}

TEST_CASE("render_test_scopes: CIE 1931 spectral locus table (§10.7.7)", "[fast][render]") {
  const auto& locus = detail::kCie1931Locus;
  REQUIRE(locus.size() == 65);
  for (size_t i = 0; i < locus.size(); ++i) {
    INFO("point " << i);
    CHECK(locus.at(i).nm == 380 + (5 * static_cast<int>(i)));
    CHECK(locus.at(i).x > 0.0F);
    CHECK(locus.at(i).x < 0.8F);  // inside the plot of §10.7.7
    CHECK(locus.at(i).y > 0.0F);
    CHECK(locus.at(i).y < 0.9F);
    CHECK(locus.at(i).x + locus.at(i).y <= 1.0F);
  }
  // Well-known points of the 2° observer: the green apex near 520 nm and the red end.
  CHECK(locus.at(28).nm == 520);
  CHECK(locus.at(28).x == Catch::Approx(0.0743).margin(1e-4));
  CHECK(locus.at(28).y == Catch::Approx(0.8338).margin(1e-4));
  CHECK(locus.back().x == Catch::Approx(0.7347).margin(1e-4));
  CHECK(locus.back().y == Catch::Approx(0.2653).margin(1e-4));
  // The BT.2020 primaries are monochromatic: 630, 532 and 467 nm lie on the locus.
  CHECK(locus.at(50).x == Catch::Approx(0.708).margin(1e-3));
  CHECK(locus.at(50).y == Catch::Approx(0.292).margin(1e-3));
}

TEST_CASE("render_test_scopes: AT1 a 203-nit flat frame fills one waveform row (FR-HDR-010)",
          "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 16);
  OwnedPicture op = grey(203.0);
  const double nits = decoded_nits(luma_code(detail::pq_oetf(203.0)));
  REQUIRE(nits == Catch::Approx(203.0).epsilon(0.01));
  const Measurements m = measure_frame(*surface, op, 0);

  // The ScopeTexture of the result (Interop::None: the pl_tex itself, RGBA16F).
  REQUIRE(m.waveform.valid);
  REQUIRE(m.vectorscope.valid);
  REQUIRE(m.cie_xy.valid);
  CHECK(m.waveform.tex.width == 1024);
  CHECK(m.waveform.tex.height == 512);
  CHECK((m.vectorscope.tex.width == 512 && m.cie_xy.tex.height == 512));
  CHECK(m.waveform.tex.format == ovt::render::DisplayTarget::OffscreenFormat::Rgba16F);
  CHECK(m.waveform.tex.interop == ovt::render::DisplayTarget::Interop::None);
  CHECK_FALSE(m.histogram_image.valid);  // §10.7.8, not this work item

  // All energy in the row for 203 nits, +- 1 row; every column has samples (1920 / 1024 per column).
  const int expected_y = detail::waveform_texel_y(detail::waveform_row(203.0, detail::WaveformAxis::LogNits));
  const Scope wf = download(*surface, ScopeKind::Waveform);
  const auto box = energy_box(wf, 0);
  INFO("energy rows " << box[1] << ".." << box[3] << ", expected " << expected_y);
  CHECK(box[0] == 0);
  CHECK(box[2] == 1023);
  CHECK(box[1] >= expected_y - 1);
  CHECK(box[3] <= expected_y + 1);
  CHECK(total(wf, 0) == Catch::Approx(static_cast<double>(m.sampled_pixels)).epsilon(0.01));
  // Luminance mode repeats the value in R, G and B.
  CHECK((wf.at(512, box[1], 0) == wf.at(512, box[1], 1) && wf.at(512, box[1], 0) == wf.at(512, box[1], 2)));

  // Grey: the vectorscope centre and the D65 point of the CIE plot, +- 2 px.
  const auto vbox = energy_box(download(*surface, ScopeKind::Vectorscope), 0);
  CHECK(vbox[0] >= 254);
  CHECK(vbox[2] <= 257);
  CHECK(vbox[1] >= 254);
  CHECK(vbox[3] <= 257);
  const auto d65 = detail::cie_uv(0.3127, 0.3290);
  const auto cbox = energy_box(download(*surface, ScopeKind::CieXy), 0);
  CHECK(cbox[0] >= static_cast<int>(d65[0]) - 2);
  CHECK(cbox[2] <= static_cast<int>(d65[0]) + 2);
  CHECK(cbox[1] >= static_cast<int>(d65[1]) - 2);
  CHECK(cbox[3] <= static_cast<int>(d65[1]) + 2);

  // PQ-code axis: the same frame lands on row 511 x PQ(203).
  REQUIRE(detail::set_scope_config(*surface, detail::ScopeConfig{detail::WaveformAxis::PqCode}));
  static_cast<void>(measure_frame(*surface, op, 1));
  const int pq_y = detail::waveform_texel_y(detail::waveform_row(nits, detail::WaveformAxis::PqCode));
  const auto pbox = energy_box(download(*surface, ScopeKind::Waveform), 0);
  CHECK(pbox[1] >= pq_y - 1);
  CHECK(pbox[3] <= pq_y + 1);
}

TEST_CASE("render_test_scopes: AT2 BT.2020 red lands on the red primary point (FR-HDR-010)", "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 16);
  OwnedPicture op = red2020();
  static_cast<void>(measure_frame(*surface, op, 0));

  // Vectorscope: the BT.2020 colour-bar red of gamut_tables.cpp, +- 2 px.
  const detail::CbCr red = detail::vectorscope_targets(detail::Gamut::Bt2020)[0];
  const auto uv = detail::vectorscope_uv(red.cb, red.cr, 1.0F);
  const auto at = detail::scope_texel(uv[0], uv[1], detail::kScopeSize);
  const Scope vs = download(*surface, ScopeKind::Vectorscope);
  const auto box = energy_box(vs, 0);
  INFO("vectorscope energy " << box[0] << "," << box[1] << " .. " << box[2] << "," << box[3] << "; red point " << at[0]
                             << "," << at[1]);
  REQUIRE(box[0] >= 0);
  CHECK(box[0] >= at[0] - 2);
  CHECK(box[2] <= at[0] + 2);
  CHECK(box[1] >= at[1] - 2);
  CHECK(box[3] <= at[1] + 2);
  // One texel holds every sample: log2(1 + 2 073 600) in RGBA16F keeps the count to about 1 %.
  CHECK(total(vs, 0) == Catch::Approx(double{kW} * kH).epsilon(0.02));

  // CIE xy: the BT.2020 red primary (0.708, 0.292), +- 2 px.
  const auto xy = detail::gamut_primaries_xy(detail::Gamut::Bt2020)[0];
  const auto cuv = detail::cie_uv(xy.x, xy.y);
  const auto cbox = energy_box(download(*surface, ScopeKind::CieXy), 0);
  CHECK(cbox[0] >= static_cast<int>(cuv[0]) - 2);
  CHECK(cbox[2] <= static_cast<int>(cuv[0]) + 2);
  CHECK(cbox[1] >= static_cast<int>(cuv[1]) - 2);
  CHECK(cbox[3] <= static_cast<int>(cuv[1]) + 2);

  // RGB parade: R near 10 000 nits, G and B at the 0.0001-nit floor (bottom row).
  REQUIRE(detail::set_scope_config(
      *surface, detail::ScopeConfig{detail::WaveformAxis::LogNits, detail::WaveformChannels::RgbParade}));
  static_cast<void>(measure_frame(*surface, op, 1));
  const Scope parade = download(*surface, ScopeKind::Waveform);
  const auto rbox = energy_box(parade, 0);
  CHECK(rbox[3] <= 1);  // 10 000 nits: the top rows
  const auto gbox = energy_box(parade, 1);
  CHECK(gbox[1] >= detail::kWaveformHeight - 2);
  const auto bbox = energy_box(parade, 2);
  CHECK(bbox[1] >= detail::kWaveformHeight - 2);

  // Gain 2 puts 100 % red off the plot: the points are dropped, not piled on the edge.
  REQUIRE(detail::set_scope_config(
      *surface, detail::ScopeConfig{detail::WaveformAxis::LogNits, detail::WaveformChannels::Luminance, 2.0F}));
  static_cast<void>(measure_frame(*surface, op, 2));
  CHECK(energy_box(download(*surface, ScopeKind::Vectorscope), 0)[0] == -1);
}

TEST_CASE("render_test_scopes: AT3 accumulating twice gives byte-identical scope textures", "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 16);
  // A frame with structure: a horizontal luma ramp over BT.2020 red-ish chroma, so every scope has a spread.
  OwnedPicture op = red2020();
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      op.set(0, x, y, 64 + ((x * 876) / (kW - 1)));
    }
  }
  std::array<std::vector<uint16_t>, 3> first;
  for (int run = 0; run < 2; ++run) {
    static_cast<void>(measure_frame(*surface, op, run));
    for (const ScopeKind kind : {ScopeKind::Waveform, ScopeKind::Vectorscope, ScopeKind::CieXy}) {
      const Scope s = download(*surface, kind);
      INFO("run " << run << " scope " << static_cast<int>(kind));
      CHECK(energy_box(s, 0)[0] >= 0);
      if (run == 0) {
        first.at(static_cast<size_t>(kind)) = s.texels;
      } else {
        CHECK(s.texels == first.at(static_cast<size_t>(kind)));
      }
    }
  }
}

// Review of #514: frames rendered back to back while a readback is in flight must not overwrite the scope textures
// that measure() is still handing out for an older measured_frame. hold_readbacks() plays the slow GPU.
TEST_CASE("render_test_scopes: back-to-back renders keep each result's scopes with its frame (§10.7.9)",
          "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 16);
  // 1 nit and 203 nits land in different waveform rows.
  OwnedPicture dark = grey(1.0);
  OwnedPicture white = grey(203.0);
  const auto row_of = [](double nits) {
    return detail::waveform_texel_y(
        detail::waveform_row(decoded_nits(luma_code(detail::pq_oetf(nits))), detail::WaveformAxis::LogNits));
  };
  const int dark_y = row_of(1.0);
  const int white_y = row_of(203.0);
  REQUIRE(std::abs(dark_y - white_y) > 2);
  const auto rows_of_shown = [&] {
    const auto box = energy_box(download(*surface, ScopeKind::Waveform), 0);
    return std::array<int, 2>{box[1], box[3]};
  };

  // A (dark) is measured and shown: its slot's scope set is the one measure() hands out.
  const Measurements a = measure_frame(*surface, dark, 0);
  REQUIRE(a.waveform.valid);
  CHECK(rows_of_shown() == std::array<int, 2>{dark_y, dark_y});

  // B and C (white) render back to back while no readback completes: B takes the free slot, and the only slot left
  // for C is A's.
  REQUIRE(detail::hold_readbacks(*surface, true));
  for (int i = 1; i <= 2; ++i) {
    white.pic.id = ovt::fb::FrameId(i, i);
    white.pic.generation = static_cast<uint64_t>(i) + 10;
    surface->set_frame(white.pic);
    static_cast<void>(surface->measure());
    surface->render(one_to_one(white));
    REQUIRE(std::string(surface->last_error()).empty());
  }
  // measure() still reports A, and A's scope textures still hold A, not C.
  const Measurements still = surface->measure();
  REQUIRE((still.valid && still.measured_frame.decode_index() == 0));
  REQUIRE(still.waveform.valid);
  CHECK(rows_of_shown() == std::array<int, 2>{dark_y, dark_y});

  // Once the readbacks land, C is the newest result. Its slot was the shown one, so it was measured without scopes and
  // says so; the next frame gets scopes again, in its own set.
  REQUIRE(detail::hold_readbacks(*surface, false));
  Measurements m = surface->measure();
  for (int tries = 0; tries < 60000 && m.measured_frame.decode_index() != 2; ++tries) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m = surface->measure();
  }
  CHECK(m.measured_frame.decode_index() == 2);
  CHECK_FALSE(m.waveform.valid);  // C's scopes were skipped
  const Measurements b = measure_frame(*surface, white, 3);
  REQUIRE(b.waveform.valid);
  CHECK(rows_of_shown() == std::array<int, 2>{white_y, white_y});
}

TEST_CASE("render_test_scopes: Scope*Png export writes the 8-bit LUT image (§10.8)", "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 16);
  OwnedPicture op = grey(203.0);
  op.pic.id = ovt::fb::FrameId(0, 0);
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  const std::filesystem::path dir = std::filesystem::temp_directory_path();
  const std::string path = (dir / "ovt_test_scope_waveform.png").string();
  // export_png measures by itself: no measure()/render() needed first.
  REQUIRE(surface->export_png(path.c_str(), ovt::render::ExportKind::ScopeWaveformPng, nullptr));
  CHECK(std::string(surface->last_error()).empty());

  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  REQUIRE(png.size() > 33);
  CHECK(std::equal(png.begin(), png.begin() + 8,
                   std::array<uint8_t, 8>{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}.begin()));
  const auto be32 = [&](size_t at) {
    return (uint32_t{png.at(at)} << 24U) | (uint32_t{png.at(at + 1)} << 16U) | (uint32_t{png.at(at + 2)} << 8U) |
           uint32_t{png.at(at + 3)};
  };
  CHECK(be32(16) == 1024);  // IHDR width
  CHECK(be32(20) == 512);   // IHDR height
  CHECK(png.at(24) == 8);   // bit depth
  CHECK(png.at(25) == 2);   // RGB
  static_cast<void>(std::remove(path.c_str()));

  // The LUT itself: the largest value maps to 255, zero to 0, R = G = B for luminance.
  std::vector<uint16_t> texels(size_t{8}, 0);
  texels[0] = texels[1] = texels[2] = 0x4A00;  // 12.0
  texels[4] = 0x4600;                          // 6.0 in R only
  const std::vector<uint8_t> rgb = detail::scope_png_rgb(texels, 2, 1);
  CHECK(rgb == std::vector<uint8_t>{255, 255, 255, 128, 0, 0});
  CHECK(detail::scope_png_rgb(std::vector<uint16_t>(4, 0), 1, 1) == std::vector<uint8_t>{0, 0, 0});

  // Without a picture there is nothing to export.
  auto empty = surface_or_skip(16, 16);
  CHECK_FALSE(empty->export_png(path.c_str(), ovt::render::ExportKind::ScopeCiePng, nullptr));
  CHECK_FALSE(std::string(empty->last_error()).empty());
}
