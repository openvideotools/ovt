// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.453 (chapter 10 §10.11): the false-colour view (§10.7.5, Table 10.7-1) and the gamut-clip overlay (§10.7.8),
// with the band fractions and out-of-gamut counts of Measurements. The patterns are synthesised from their
// expected.json (tests/corpora/patterns/, WI-1.413) with the generator's limited-range codes, as in test_measure.cpp.
// The image goldens are tests/golden/render/false_color_patches.png and gamut_clip_p3.png (§10.9.2 items 4 and 6:
// OVT_UPDATE_GOLDEN=1 rewrites them, never on CI); both use the patterns at a quarter of their size, so each golden is
// 480 x 270 with the same patch layout.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include <libplacebo/colorspace.h>

#include "measure/measure.h"
#include "ovt/render/video_surface.h"
#include "png_io.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::Measurements;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::OutputPixels;
using ovt::render::test::OwnedPicture;
using ovt::render::test::RgbImage;
using ovt::render::test::surface_or_skip;
namespace detail = ovt::render::detail;

nlohmann::json expected(const std::string& pattern) {
  std::ifstream in(std::string(OVT_PATTERNS_DIR) + "/" + pattern + ".expected.json");
  REQUIRE(in.good());
  return nlohmann::json::parse(in);
}

// A 10-bit limited 4:2:0 picture of w x h filled with (y, cb, cr).
OwnedPicture picture(uint32_t w, uint32_t h, uint8_t primaries, uint8_t transfer, uint8_t matrix,
                     std::array<uint32_t, 3> fill) {
  OwnedPicture op(w, h, 10, 1, 2);
  op.pic.color.primaries = primaries;
  op.pic.color.transfer = transfer;
  op.pic.color.matrix = matrix;
  for (uint8_t p = 0; p < 3; ++p) {
    op.fill_plane(p, fill.at(p));
  }
  return op;
}

// Fills columns [x0, x1) of every row with (y, cb, cr); x0 and x1 are even, so the chroma columns are whole.
void fill_columns(OwnedPicture& op, uint32_t x0, uint32_t x1, std::array<uint32_t, 3> ycc) {
  for (uint32_t y = 0; y < op.pic.coded_height; ++y) {
    for (uint32_t x = x0; x < x1; ++x) {
      op.set(0, x, y, ycc[0]);
      if (x % 2 == 0 && y % 2 == 0) {
        op.set(1, x / 2, y / 2, ycc[1]);
        op.set(2, x / 2, y / 2, ycc[2]);
      }
    }
  }
}

// tests/corpora/patterns/pq_patches_1000 frame 0 at 1/`div` of its size: six equal-width columns, BT.2020 PQ.
OwnedPicture pq_patches(const nlohmann::json& exp, uint32_t div) {
  OwnedPicture op =
      picture(exp.at("width").get<uint32_t>() / div, exp.at("height").get<uint32_t>() / div, 9, 16, 9, {64, 512, 512});
  const auto& columns = exp.at("patch_columns");
  const auto& codes = exp.at("codes_limited");
  for (size_t p = 0; p < columns.size(); ++p) {
    fill_columns(op, columns[p][0].get<uint32_t>() / div, columns[p][1].get<uint32_t>() / div,
                 {codes[p].get<uint32_t>(), 512, 512});
  }
  return op;
}

// tests/corpora/patterns/p3_saturation frame 0 at 1/`div` of its size: eight stripes, Display P3 primaries (H.273
// code 12), sRGB transfer (13), BT.709 matrix, limited range.
OwnedPicture p3_saturation(const nlohmann::json& exp, uint32_t div) {
  OwnedPicture op =
      picture(exp.at("width").get<uint32_t>() / div, exp.at("height").get<uint32_t>() / div, 12, 13, 1, {64, 512, 512});
  for (const auto& stripe : exp.at("stripes")) {
    const auto& c = stripe.at("codes_ycbcr_limited");
    fill_columns(op, stripe.at("columns")[0].get<uint32_t>() / div, stripe.at("columns")[1].get<uint32_t>() / div,
                 {c[0].get<uint32_t>(), c[1].get<uint32_t>(), c[2].get<uint32_t>()});
  }
  return op;
}

ViewState one_to_one(const OwnedPicture& op) {
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = op.pic.coded_width / 2.0;
  view.pan_y = op.pic.coded_height / 2.0;
  return view;
}

void set_frame(VideoSurface& surface, OwnedPicture& op, int index, uint64_t generation) {
  op.pic.id = ovt::fb::FrameId(index, index);
  op.pic.generation = generation;
  surface.set_frame(op.pic);
  REQUIRE(std::string(surface.last_error()).empty());
}

bool same_frame(const ovt::fb::FrameId& a, const ovt::fb::FrameId& b) {
  return a.decode_index() == b.decode_index() && a.display_index() == b.display_index();
}

// The §10.7.9 consumer loop (as in test_measure.cpp): ask, render, poll until the result for `frame` is back.
Measurements measure_frame(VideoSurface& surface, const ViewState& view, const ovt::fb::FrameId& frame) {
  static_cast<void>(surface.measure());
  surface.render(view);
  REQUIRE(std::string(surface.last_error()).empty());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  Measurements m = surface.measure();
  while (!(m.valid && same_frame(m.measured_frame, frame)) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m = surface.measure();
  }
  REQUIRE(m.valid);
  REQUIRE(same_frame(m.measured_frame, frame));
  return m;
}

RgbImage render_rgb(VideoSurface& surface, const ViewState& view) {
  surface.render(view);
  REQUIRE(std::string(surface.last_error()).empty());
  OutputPixels px;
  REQUIRE(detail::download_output(surface, px));
  REQUIRE(px.texel_bytes == 4);
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

std::array<int, 3> rgb_at(const RgbImage& img, int x, int y) {
  const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(img.width)) + static_cast<size_t>(x)) * 3;
  return {img.rgb.at(i), img.rgb.at(i + 1), img.rgb.at(i + 2)};
}

// Every pixel of columns [x0, x1) is within `tol` codes of `want` per channel.
void check_uniform(const RgbImage& img, int x0, int x1, std::array<uint8_t, 3> want, int tol) {
  int worst = 0;
  for (int y = 0; y < img.height; ++y) {
    for (int x = x0; x < x1; ++x) {
      const std::array<int, 3> got = rgb_at(img, x, y);
      for (size_t c = 0; c < 3; ++c) {
        worst = std::max(worst, std::abs(got.at(c) - int{want.at(c)}));
      }
    }
  }
  const std::array<int, 3> centre = rgb_at(img, (x0 + x1) / 2, img.height / 2);
  INFO("columns " << x0 << ".." << x1 << " want " << int{want[0]} << "," << int{want[1]} << "," << int{want[2]}
                  << " centre " << centre[0] << "," << centre[1] << "," << centre[2]);
  CHECK(worst <= tol);
}

// §10.9.2 items 4 and 6 (#465, #469): a pixel differs when any 8-bit channel is more than 2 codes off the golden, and
// at most 0.1 % of the pixels may differ. OVT_UPDATE_GOLDEN=1 rewrites the golden (never when CI is set).
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
  const RgbImage want = read.value_or(RgbImage{});
  REQUIRE(want.width == actual.width);
  REQUIRE(want.height == actual.height);
  size_t differing = 0;
  for (size_t p = 0; p < actual.rgb.size(); p += 3) {
    for (size_t c = 0; c < 3; ++c) {
      if (std::abs(int{actual.rgb.at(p + c)} - int{want.rgb.at(p + c)}) > 2) {
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

}  // namespace

TEST_CASE("render_test_false_color: Table 10.7-1 bands, colours and the gamut helpers", "[fast][render]") {
  // Bands on max(R,G,B), lower bound inclusive.
  CHECK(detail::false_color_band(0.0) == 0);
  CHECK(detail::false_color_band(0.999) == 0);
  CHECK(detail::false_color_band(1.0) == 1);
  CHECK(detail::false_color_band(99.99) == 1);
  CHECK(detail::false_color_band(100.0) == 2);
  CHECK(detail::false_color_band(202.9) == 2);
  CHECK(detail::false_color_band(203.0) == 3);
  CHECK(detail::false_color_band(1000.0) == 4);
  CHECK(detail::false_color_band(3999.0) == 4);
  CHECK(detail::false_color_band(4000.0) == 5);
  CHECK(detail::false_color_band(10000.0) == 5);

  using Rgb = std::array<uint8_t, 3>;
  CHECK(detail::false_color_rgb(0.5) == Rgb{0x1F, 0x1F, 0x5C});
  CHECK(detail::false_color_rgb(1.0) == Rgb{0x40, 0x40, 0x40});   // grey ramp start
  CHECK(detail::false_color_rgb(10.0) == Rgb{0x80, 0x80, 0x80});  // log midpoint of 1..100
  CHECK(detail::false_color_rgb(99.999) == Rgb{0xC0, 0xC0, 0xC0});
  CHECK(detail::false_color_rgb(150.0) == Rgb{0x2E, 0xCC, 0x40});
  CHECK(detail::false_color_rgb(500.0) == Rgb{0xFF, 0xDC, 0x00});
  CHECK(detail::false_color_rgb(2000.0) == Rgb{0xFF, 0x85, 0x1B});
  CHECK(detail::false_color_rgb(5000.0) == Rgb{0xFF, 0x41, 0x36});

  // §10.7.8 overlay: grey = PQ(Y); tints at alpha 0x99, P3 (cyan) over BT.709 (magenta).
  const auto grey = static_cast<uint8_t>(std::lround(detail::pq_oetf(203.0) * 255.0));
  CHECK(detail::gamut_clip_rgb(203.0, false, false) == Rgb{grey, grey, grey});
  const auto mix = [grey](int tint) {
    return static_cast<uint8_t>(std::lround((grey + ((tint - grey) * 153.0 / 255.0))));
  };
  CHECK(detail::gamut_clip_rgb(203.0, true, false) == Rgb{mix(255), mix(0), mix(255)});
  CHECK(detail::gamut_clip_rgb(203.0, true, true) == Rgb{mix(0), mix(255), mix(255)});
  CHECK(detail::gamut_clip_rgb(0.0, true, false) == Rgb{153, 0, 153});

  // Container peaks in M units (1.0 = 203 nits).
  struct pl_color_space c = pl_color_space_unknown;
  c.primaries = PL_COLOR_PRIM_BT_2020;
  c.transfer = PL_COLOR_TRC_PQ;
  CHECK(detail::gamut_peak(c) == Catch::Approx(10000.0 / 203.0));
  c.hdr.max_luma = 1000.0F;  // a mastering peak does not narrow the PQ container
  CHECK(detail::gamut_peak(c) == Catch::Approx(10000.0 / 203.0));
  c = pl_color_space_unknown;
  c.primaries = PL_COLOR_PRIM_BT_2020;
  c.transfer = PL_COLOR_TRC_HLG;
  CHECK(detail::gamut_peak(c) == Catch::Approx(1000.0 / 203.0));
  c.transfer = PL_COLOR_TRC_SRGB;
  CHECK(detail::gamut_peak(c) == Catch::Approx(1.0));

  // P3 red in BT.709 linear RGB, as tests/corpora/patterns/p3_saturation.expected.json states it.
  const std::array<float, 9> m = detail::gamut_matrix(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_PRIM_BT_709);
  // 1e-3: the generator and libplacebo round the D65 white point differently.
  CHECK(m[0] == Catch::Approx(1.224184).margin(1e-3));  // column 0 = image of (1, 0, 0)
  CHECK(m[1] == Catch::Approx(-0.042031).margin(1e-3));
  CHECK(m[2] == Catch::Approx(-0.019625).margin(1e-3));
  const std::array<float, 9> id = detail::gamut_matrix(PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_BT_709);
  for (size_t i = 0; i < id.size(); ++i) {
    CHECK(id.at(i) == Catch::Approx(i % 4 == 0 ? 1.0 : 0.0).margin(1e-6));
  }

  ViewState v;
  CHECK(detail::analysis_view(v) == detail::AnalysisView::None);
  v.gamut_clip_overlay = true;
  CHECK(detail::analysis_view(v) == detail::AnalysisView::GamutClip);
  v.false_color = true;
  CHECK(detail::analysis_view(v) == detail::AnalysisView::FalseColor);
}

TEST_CASE("render_test_false_color: AT1 PQ patches are uniformly their band colour (golden)",
          "[fast][render][vulkan][golden]") {
  const nlohmann::json exp = expected("pq_patches_1000");
  constexpr uint32_t kDiv = 4;
  OwnedPicture op = pq_patches(exp, kDiv);
  auto surface = surface_or_skip(static_cast<int>(op.pic.coded_width), static_cast<int>(op.pic.coded_height));
  set_frame(*surface, op, 0, 1);
  ViewState view = one_to_one(op);
  view.false_color = true;
  const RgbImage img = render_rgb(*surface, view);

  // Each patch, every pixel, is the colour of the band its decoded luminance falls in (the 203.7-nit patch is band 3:
  // the generator's code sits just above the 203-nit edge; see band_fractions in the expected.json).
  const auto& columns = exp.at("patch_columns");
  const auto& decoded = exp.at("decoded_nits_from_limited_codes");
  const auto& fractions = exp.at("band_fractions");
  for (size_t p = 0; p < columns.size(); ++p) {
    const double nits = decoded[p].get<double>();
    INFO("patch " << p << " (" << nits << " nits, band " << detail::false_color_band(nits) << ")");
    CHECK(fractions[static_cast<size_t>(detail::false_color_band(nits))].get<double>() > 0.0);
    check_uniform(img, columns[p][0].get<int>() / static_cast<int>(kDiv),
                  columns[p][1].get<int>() / static_cast<int>(kDiv), detail::false_color_rgb(nits), 1);
  }
  check_golden(img, "false_color_patches.png");

  // Deterministic: the same frame renders to the same bytes.
  const RgbImage again = render_rgb(*surface, view);
  CHECK(again.rgb == img.rgb);

  // false colour wins over the gamut-clip overlay.
  view.gamut_clip_overlay = true;
  CHECK(render_rgb(*surface, view).rgb == img.rgb);
}

TEST_CASE("render_test_false_color: band fractions of the PQ patches (§10.7.5)", "[fast][render][vulkan]") {
  const nlohmann::json exp = expected("pq_patches_1000");
  auto surface = surface_or_skip(16, 16);
  OwnedPicture op = pq_patches(exp, 1);
  set_frame(*surface, op, 0, 1);
  ViewState view = one_to_one(op);
  view.false_color = true;  // the view and the measurement share one M and Q in the same render
  const Measurements m = measure_frame(*surface, view, op.pic.id);
  REQUIRE(m.sampled_pixels == 1920U * 1080U);
  const auto& fractions = exp.at("band_fractions");
  double total = 0;
  for (size_t b = 0; b < 6; ++b) {
    INFO("band " << b);
    const auto got = static_cast<double>(m.false_color_band_fraction[b]);
    CHECK(got == Catch::Approx(fractions[b].get<double>()).margin(1e-6));
    total += got;
  }
  CHECK(total == Catch::Approx(1.0).margin(1e-6));
  CHECK(m.out_of_gamut_709 == exp.at("out_of_gamut_709_samples").get<uint32_t>());  // greys are in every gamut
  CHECK(m.out_of_gamut_p3 == 0);
}

TEST_CASE("render_test_false_color: AT2 P3 saturation counts outside BT.709 (§10.7.8)", "[fast][render][vulkan]") {
  const nlohmann::json exp = expected("p3_saturation");
  auto surface = surface_or_skip(16, 16);
  OwnedPicture op = p3_saturation(exp, 1);
  set_frame(*surface, op, 0, 1);
  const Measurements m = measure_frame(*surface, one_to_one(op), op.pic.id);
  REQUIRE(m.sampled_pixels == 1920U * 1080U);
  const auto saturated = exp.at("saturated_area_samples").get<uint32_t>();
  CHECK(static_cast<double>(m.out_of_gamut_709) >= 0.99 * saturated);
  CHECK(m.out_of_gamut_709 <= saturated);  // the grey and white stripes are inside BT.709
  // P3 content stays inside P3, except where 4:2:0 chroma is interpolated across a stripe edge: the luma column next
  // to an edge mixes two stripes' Cb/Cr, which can leave P3. At most one column per edge (seven edges).
  const auto height = op.pic.coded_height;
  CHECK(m.out_of_gamut_p3 <= 7 * height);

  // Each stripe alone, 2 columns in from its edges (away from the chroma interpolation): all or nothing, as the
  // expected.json says.
  int index = 1;
  for (const auto& stripe : exp.at("stripes")) {
    const int x0 = stripe.at("columns")[0].get<int>() + 2;
    const int x1 = stripe.at("columns")[1].get<int>() - 2;
    op.pic.crop = ovt::fb::Rect(x0, 0, x1 - x0, static_cast<int>(height));
    set_frame(*surface, op, index, static_cast<uint64_t>(index) + 1);
    ++index;
    const Measurements ms = measure_frame(*surface, one_to_one(op), op.pic.id);
    INFO("stripe " << stripe.at("name").get<std::string>());
    const uint32_t area = static_cast<uint32_t>(x1 - x0) * height;
    CHECK(ms.sampled_pixels == area);
    CHECK(ms.out_of_gamut_709 == (stripe.at("outside_709").get<bool>() ? area : 0U));
    CHECK(ms.out_of_gamut_p3 == 0);
  }
}

TEST_CASE("render_test_false_color: gamut-clip overlay of the P3 stripes (golden)", "[fast][render][vulkan][golden]") {
  const nlohmann::json exp = expected("p3_saturation");
  constexpr uint32_t kDiv = 4;
  OwnedPicture op = p3_saturation(exp, kDiv);
  auto surface = surface_or_skip(static_cast<int>(op.pic.coded_width), static_cast<int>(op.pic.coded_height));
  set_frame(*surface, op, 0, 1);
  ViewState view = one_to_one(op);
  view.gamut_clip_overlay = true;
  const RgbImage img = render_rgb(*surface, view);

  // The six saturated stripes are magenta-tinted, grey50 and white are plain grey: PQ of their luminance.
  const pl_matrix3x3 rgb2xyz = pl_get_rgb2xyz_matrix(pl_raw_primaries_get(PL_COLOR_PRIM_DISPLAY_P3));
  for (const auto& stripe : exp.at("stripes")) {
    const auto& p3 = stripe.at("p3_rgb");
    double y = 0;
    for (size_t c = 0; c < 3; ++c) {
      const double v = p3[c].get<double>();
      const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);  // sRGB EOTF
      y += static_cast<double>(rgb2xyz.m[1][c]) * linear * 203.0;
    }
    const bool outside = stripe.at("outside_709").get<bool>();
    INFO("stripe " << stripe.at("name").get<std::string>() << " Y " << y << " nits");
    // 3 codes: Y comes from the stripe's rounded 10-bit codes, not from the ideal P3 values used here. The columns next
    // to a stripe edge mix two stripes' chroma (4:2:0) and are left to the golden.
    check_uniform(img, (stripe.at("columns")[0].get<int>() / static_cast<int>(kDiv)) + 2,
                  (stripe.at("columns")[1].get<int>() / static_cast<int>(kDiv)) - 2,
                  detail::gamut_clip_rgb(y, outside, false), 3);
  }
  check_golden(img, "gamut_clip_p3.png");
}

TEST_CASE("render_test_false_color: BT.2020 green is outside P3 and tinted cyan (§10.7.8)", "[fast][render][vulkan]") {
  // 128 x 64 BT.2020 PQ: left half the BT.2020 green primary at 100 nits (R'G'B' = (0, PQ(100), 0) through the
  // BT.2020 NCL matrix, limited range), right half neutral 100 nits.
  OwnedPicture op = picture(128, 64, 9, 16, 9, {509, 512, 512});
  fill_columns(op, 0, 64, {366, 348, 303});
  auto surface = surface_or_skip(128, 64);
  set_frame(*surface, op, 0, 1);
  ViewState view = one_to_one(op);
  view.gamut_clip_overlay = true;
  const Measurements m = measure_frame(*surface, view, op.pic.id);
  CHECK(m.out_of_gamut_p3 == 64U * 64U);
  CHECK(m.out_of_gamut_709 == 64U * 64U);

  const RgbImage img = render_rgb(*surface, view);
  const std::array<int, 3> green = rgb_at(img, 32, 32);
  const std::array<int, 3> neutral = rgb_at(img, 96, 32);
  CHECK(green[1] > 200);  // cyan tint: G and B high, R low
  CHECK(green[2] > 200);
  CHECK(green[0] < 100);
  CHECK(neutral[0] == neutral[1]);  // untinted grey
  CHECK(neutral[1] == neutral[2]);
  const auto grey = static_cast<int>(std::lround(detail::pq_oetf(99.91) * 255.0));
  CHECK(std::abs(neutral[0] - grey) <= 2);
}
