// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.431 (chapter 10 §10.11): the overlay layer, the heatmap LUTs and the heatmap / fill / hatch passes, composited
// through pl_overlay (§10.5, §10.5.1, §10.5.2, §10.5.6) and FR-HDR-014 (overlay white at SDR reference white).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fixtures/make_synthetic.h"
#include "luts/hot.h"
#include "luts/turbo.h"
#include "luts/viridis.h"
#include "overlay/overlay_layer.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"
#include "png_io.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::DisplayTarget;
using ovt::render::HeatmapSource;
using ovt::render::Lut;
using ovt::render::OverlayBuilder;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::OutputPixels;
using ovt::render::test::OwnedPicture;
using ovt::render::test::RgbImage;
using ovt::render::test::surface_or_skip;

// The default synthetic fixture: 80 x 64 cells = 320 x 256 luma samples, drawn 1:1 into a 320 x 256 drawable.
constexpr int kW = 320;
constexpr int kH = 256;

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

// A flat mid-grey 8-bit 4:2:0 BT.709 picture the size of the fixture.
OwnedPicture grey_picture() {
  OwnedPicture op(kW, kH, 8, 1, 1);
  op.fill_plane(0, 126);
  op.fill_plane(1, 128);
  op.fill_plane(2, 128);
  op.pic.generation = 1;
  return op;
}

ViewState one_to_one() {
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = kW / 2.0;
  view.pan_y = kH / 2.0;
  view.dither = false;  // the overlay colours are what these tests check
  return view;
}

OutputPixels render_with(VideoSurface& surface, const OverlayConfig* cfg, const ViewState& view) {
  if (cfg != nullptr) {
    const std::vector<uint8_t> fa = ovt::render::test::make_synthetic();
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

// RGB of device pixel (x, y) of an RGBA8 download.
std::array<int, 3> rgb_at(const OutputPixels& px, int x, int y) {
  const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(px.width)) + static_cast<size_t>(x)) * 4;
  return {px.bytes.at(i), px.bytes.at(i + 1), px.bytes.at(i + 2)};
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

// §10.9.2 item 4: at most 0.1 % of the pixels may differ. A pixel differs when any channel is more than 2 codes off,
// which absorbs the last-bit differences of blending and sRGB conversion between Vulkan drivers (lavapipe, MoltenVK).
void check_golden(const RgbImage& actual, const std::string& name) {
  const std::string golden = std::string(OVT_RENDER_GOLDEN_DIR) + "/" + name;
  const char* update = std::getenv("OVT_UPDATE_GOLDEN");  // NOLINT(concurrency-mt-unsafe): test setup
  const char* ci = std::getenv("CI");                     // NOLINT(concurrency-mt-unsafe)
  if (update != nullptr && std::string(update) == "1" && (ci == nullptr || std::string(ci).empty())) {
    REQUIRE(ovt::render::test::write_png(golden, actual));
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
    static_cast<void>(ovt::render::test::write_png(out, actual));
    FAIL(differing << " of " << pixels << " pixels differ from " << golden << "; this render: " << out);
  }
}

// ST 2084 EOTF: normalised PQ signal -> cd/m².
double pq_to_nits(double e) {
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 4096.0 * 128.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 4096.0 * 32.0;
  constexpr double c3 = 2392.0 / 4096.0 * 32.0;
  const double p = std::pow(e, 1.0 / m2);
  return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - (c3 * p)), 1.0 / m1);
}

double half_to_double(uint16_t h) {
  const int exp = (h >> 10U) & 0x1F;
  const double mant = h & 0x3FFU;
  const double sign = (h & 0x8000U) != 0 ? -1.0 : 1.0;
  if (exp == 0) {
    return sign * std::ldexp(mant, -24);
  }
  return sign * std::ldexp(1.0 + (mant / 1024.0), exp - 15);
}

// The first cell of the fixture whose QP is 51 (full scale).
std::pair<int, int> cell_with_qp51() {
  for (uint32_t cy = 0; cy < 64; ++cy) {
    for (uint32_t cx = 0; cx < 80; ++cx) {
      if (ovt::render::test::expected_qp(cx, cy) == 51) {
        return {static_cast<int>(cx), static_cast<int>(cy)};
      }
    }
  }
  FAIL("fixture has no QP 51 cell");
  return {0, 0};
}

}  // namespace

TEST_CASE("render_test_overlay_luts: generated tables and fill palette (§10.5.1, §10.5.2)", "[fast][render][overlay]") {
  using ovt::render::luts::kHot;
  using ovt::render::luts::kTurbo;
  using ovt::render::luts::kViridis;
  // matplotlib's viridis endpoints (0.267004, 0.004874, 0.329415) and (0.993248, 0.906157, 0.143936).
  CHECK(kViridis[0] == 68);
  CHECK(kViridis[1] == 1);
  CHECK(kViridis[2] == 84);
  CHECK(kViridis[1020] == 253);
  CHECK(kViridis[1021] == 231);
  CHECK(kViridis[1022] == 37);
  // hot ends in white: the overlay white of the FR-HDR-014 test.
  CHECK(kHot[1020] == 255);
  CHECK(kHot[1021] == 255);
  CHECK(kHot[1022] == 255);
  for (size_t i = 3; i < 1024; i += 4) {
    REQUIRE(kViridis.at(i) == 255);
    REQUIRE(kTurbo.at(i) == 255);
  }
  // Turbo polynomial at x = 128 / 255: (0.5946, 0.9807, 0.3101) -> (152, 250, 79).
  CHECK(kTurbo[size_t{128} * 4] == 152);
  CHECK(kTurbo[(size_t{128} * 4) + 1] == 250);
  CHECK(kTurbo[(size_t{128} * 4) + 2] == 79);

  const auto& pal = ovt::render::detail::kFillPalette;
  CHECK(pal[0] == 0x2ECC4066U);   // intra fill, 40 %
  CHECK(pal[1] == 0xFF851B4DU);   // L0 fill alpha 0x4D
  CHECK(pal[4] == 0xAAAAAA99U);   // skip hatch
  CHECK(pal[22] == 0xE6194B66U);  // ref slot 0
  CHECK(pal[29] == 0xF032E666U);  // ref slot 7
}

// Acceptance test 1, plus determinism (§10.9.2 item 5).
TEST_CASE("render_test_overlay_heatmap_qp_viridis golden (§10.5.5)", "[fast][render][vulkan][overlay]") {
  const OwnedPicture pic = grey_picture();
  auto surface = surface_or_skip(kW, kH);
  surface->set_frame(pic.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::Heatmap);
  cfg.heatmap = HeatmapSource::QP;
  cfg.lut = Lut::Viridis;  // heatmap_alpha 0.6 (default)
  const ViewState view = one_to_one();
  const OutputPixels first = render_with(*surface, &cfg, view);
  const OutputPixels second = render_with(*surface, &cfg, view);
  CHECK(first.bytes == second.bytes);
  check_golden(to_rgb(first), "heatmap_qp_viridis.png");

  // Cell (0, 0) has QP -1 ("not applicable"): alpha 0, so it shows the picture; every other cell is tinted.
  const OutputPixels plain = render_with(*surface, nullptr, view);
  CHECK(rgb_at(first, 1, 1) == rgb_at(plain, 1, 1));
  CHECK(rgb_at(first, 5, 1) != rgb_at(plain, 5, 1));
  // QP 0 cells are viridis[0] (dark purple, B > G), QP 51 cells viridis[255] (yellow, R and G > B).
  const auto [hx, hy] = cell_with_qp51();
  const auto hot = rgb_at(first, (hx * 4) + 1, (hy * 4) + 1);
  CHECK(hot[0] > hot[2]);
  CHECK(hot[1] > hot[2]);
  // Heatmap off again: the same surface draws the plain picture.
  cfg.layers = 0;
  CHECK(render_with(*surface, &cfg, view).bytes == plain.bytes);
}

TEST_CASE("render_test_overlay fill and hatch passes (§10.5.2, §10.5.6)", "[fast][render][vulkan][overlay]") {
  const OwnedPicture pic = grey_picture();
  auto surface = surface_or_skip(kW, kH);
  surface->set_frame(pic.pic);
  const ViewState view = one_to_one();
  const OutputPixels plain = render_with(*surface, nullptr, view);

  SECTION("PredFill tints by prediction kind and list") {
    OverlayConfig cfg;
    cfg.layers = bit(OverlayLayer::PredFill);
    const OutputPixels px = render_with(*surface, &cfg, view);
    const auto intra = rgb_at(px, (1 * 4) + 1, 1);  // cell (1, 0): Intra -> #2ECC40
    CHECK(intra[1] > intra[0]);
    CHECK(intra[1] > intra[2]);
    const auto l0 = rgb_at(px, (4 * 4) + 1, (3 * 4) + 1);  // cell (4, 3): ref0 = 0, no ref1 -> L0 #FF851B
    CHECK(l0[0] > l0[1]);
    CHECK(l0[1] > l0[2]);
    const auto bi = rgb_at(px, (10 * 4) + 1, (3 * 4) + 1);  // cell (10, 3): ref0 = 0, ref1 = 1 -> bi #00E5FF
    CHECK(bi[2] > bi[0]);
    CHECK(bi[1] > bi[0]);
  }
  SECTION("RefIdxFill colours by reference slot") {
    OverlayConfig cfg;
    cfg.layers = bit(OverlayLayer::RefIdxFill);
    const OutputPixels px = render_with(*surface, &cfg, view);
    const auto slot1 = rgb_at(px, (2 * 4) + 1, (3 * 4) + 1);  // cell (2, 3): ref0 = 1 -> slot 1 #3CB44B
    CHECK(slot1[1] > slot1[0]);
    CHECK(slot1[1] > slot1[2]);
    // cell (3, 3): ref0 = -1, ref1 = -1 -> no tint.
    CHECK(rgb_at(px, (3 * 4) + 1, (3 * 4) + 1) == rgb_at(plain, (3 * 4) + 1, (3 * 4) + 1));
  }
  SECTION("SkipHatch draws 45° lines with a 4 px period on skip cells only") {
    OverlayConfig cfg;
    cfg.layers = bit(OverlayLayer::SkipHatch);
    const OutputPixels px = render_with(*surface, &cfg, view);
    // Cell (7, 5) is Skip, cells with cx >= 40 belong to the skip block 1; cell (10, 10) is plain inter.
    for (const auto& [cx, cy] : {std::pair{7, 5}, std::pair{45, 10}}) {
      for (int y = cy * 4; y < (cy * 4) + 4; ++y) {
        for (int x = cx * 4; x < (cx * 4) + 4; ++x) {
          INFO("pixel " << x << "," << y);
          CHECK((rgb_at(px, x, y) != rgb_at(plain, x, y)) == ((x + y) % 4 == 0));
        }
      }
    }
    for (int y = 40; y < 44; ++y) {
      for (int x = 40; x < 44; ++x) {
        CHECK(rgb_at(px, x, y) == rgb_at(plain, x, y));
      }
    }
  }
  SECTION("the overlay stays inside the picture rect") {
    OverlayConfig cfg;
    cfg.layers = bit(OverlayLayer::PredFill);
    ViewState shifted = view;
    shifted.pan_x += 40;  // the picture's right 40 px leave the drawable, 40 px of background appear on the right
    const OutputPixels px = render_with(*surface, &cfg, shifted);
    const OutputPixels bg = render_with(*surface, nullptr, shifted);
    CHECK(rgb_at(px, kW - 5, 100) == rgb_at(bg, kW - 5, 100));
  }
}

// Acceptance test 2 (FR-HDR-014): overlay white is SDR reference white (203 cd/m²) on HDR targets.
// SPEC-ASSUMPTION(#466): the measurement texture M of §10.7.1 arrives with WI-4.452; until then the check reads
// the two outputs that share M's scaling: the ScRgbLinear16F target (linear BT.709, 1.0 = 203 cd/m², the same
// PL_COLOR_SDR_WHITE normalisation as M) and the Pq10 target, whose code values are decoded with the ST 2084 EOTF.
TEST_CASE("render_test_overlay white is 203 nits on HDR targets (FR-HDR-014)", "[fast][render][vulkan][overlay]") {
  const OwnedPicture pic = grey_picture();
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::Heatmap);
  cfg.heatmap = HeatmapSource::QP;
  cfg.lut = Lut::Hot;  // QP 51 -> hot[255] = white
  cfg.heatmap_alpha = 1.0F;
  const auto [cx, cy] = cell_with_qp51();
  const int x = (cx * 4) + 2;
  const int y = (cy * 4) + 2;
  const ViewState view = one_to_one();

  SECTION("linear target: 1.0") {
    auto surface = surface_or_skip(kW, kH, DisplayTarget::OffscreenFormat::Rgba16F);
    surface->set_frame(pic.pic);
    const OutputPixels px = render_with(*surface, &cfg, view);
    REQUIRE(px.texel_bytes == 8);
    const size_t i = ((static_cast<size_t>(y) * kW) + static_cast<size_t>(x)) * 8;
    for (size_t c = 0; c < 3; ++c) {
      const auto h = static_cast<uint16_t>(px.bytes.at(i + (2 * c)) | (px.bytes.at(i + (2 * c) + 1) << 8U));
      INFO("channel " << c);
      CHECK(std::abs(half_to_double(h) - 1.0) < 0.005);
    }
  }
  SECTION("PQ target: 203 cd/m²") {
    auto surface = surface_or_skip(kW, kH, DisplayTarget::OffscreenFormat::Rgb10A2);
    surface->set_frame(pic.pic);
    const OutputPixels px = render_with(*surface, &cfg, view);
    REQUIRE(px.texel_bytes == 4);
    const size_t i = ((static_cast<size_t>(y) * kW) + static_cast<size_t>(x)) * 4;
    const uint32_t v = uint32_t{px.bytes.at(i)} | (uint32_t{px.bytes.at(i + 1)} << 8U) |
                       (uint32_t{px.bytes.at(i + 2)} << 16U) | (uint32_t{px.bytes.at(i + 3)} << 24U);
    for (unsigned c = 0; c < 3; ++c) {
      const double nits = pq_to_nits(static_cast<double>((v >> (10U * c)) & 0x3FFU) / 1023.0);
      INFO("channel " << c << ": " << nits << " cd/m²");
      CHECK(std::abs(nits - 203.0) < 203.0 * 0.01);  // one 10-bit PQ code near 203 nits is ~0.5 %
    }
  }
}
