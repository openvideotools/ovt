// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.410 (chapter 10 §10.11): compare modes of slots A and B (§10.2.4, §10.6.4): layout, flicker phase, slot B
// conversion (RND-0010/RND-0011), the difference view in the code-value domain with its region PSNR, side-by-side,
// split, overlay and flicker, and the goldens diff_gain8.png and split_v.png (§10.9.2).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "luts/turbo.h"
#include "ovt/render/video_surface.h"
#include "pipeline/compare.h"
#include "png_io.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::CompareMode;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::IRect;
using ovt::render::detail::OutputPixels;
using ovt::render::test::OwnedPicture;
using ovt::render::test::RgbImage;
using ovt::render::test::surface_or_skip;
using Kind = CompareMode::Kind;

constexpr int kW = 64;
constexpr int kH = 32;

// A 10-bit 4:2:0 limited BT.709 picture of kW x kH with constant planes.
OwnedPicture flat10(uint32_t y, uint32_t u = 512, uint32_t v = 512, uint64_t generation = 1) {
  OwnedPicture p(kW, kH, 10, 1, 2);
  p.fill_plane(0, y);
  p.fill_plane(1, u);
  p.fill_plane(2, v);
  p.pic.generation = generation;
  return p;
}

ViewState view_1to1() {
  ViewState v;
  v.pan_x = kW / 2.0;
  v.pan_y = kH / 2.0;
  v.dither = false;
  return v;
}

struct Rgba8Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
};

Rgba8Image render_image(VideoSurface& s, const ViewState& v) {
  s.render(v);
  OutputPixels out;
  REQUIRE(ovt::render::detail::download_output(s, out));
  REQUIRE(out.texel_bytes == 4);
  return Rgba8Image{out.width, out.height, out.bytes};
}

// The golden check of the WI-2.431 overlay tests: at most 0.1 % of the pixels may have a channel more than 2 codes off
// (§10.9.2 item 4; SPEC-ASSUMPTION(#469)). OVT_UPDATE_GOLDEN=1 rewrites the golden, never on CI.
void check_golden(const Rgba8Image& img, const char* name) {
  RgbImage actual{img.width, img.height, {}};
  for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
    actual.rgb.insert(actual.rgb.end(), img.rgba.begin() + static_cast<std::ptrdiff_t>(i),
                      img.rgba.begin() + static_cast<std::ptrdiff_t>(i + 3));
  }
  const std::string golden = std::string(OVT_RENDER_GOLDEN_DIR) + "/" + name;
  const char* update = std::getenv("OVT_UPDATE_GOLDEN");  // NOLINT(concurrency-mt-unsafe): test setup
  const char* ci = std::getenv("CI");                     // NOLINT(concurrency-mt-unsafe)
  if (update != nullptr && std::string(update) == "1" && (ci == nullptr || std::string(ci).empty())) {
    REQUIRE(ovt::render::test::write_png(golden, actual));
    return;
  }
  const RgbImage expected = ovt::render::test::read_png(golden).value_or(RgbImage{});
  REQUIRE((expected.width == actual.width && expected.height == actual.height));
  size_t differing = 0;
  for (size_t p = 0; p < actual.rgb.size(); p += 3) {
    const auto off = [&](size_t c) { return std::abs(int{actual.rgb.at(p + c)} - int{expected.rgb.at(p + c)}) > 2; };
    differing += (off(0) || off(1) || off(2)) ? 1U : 0U;
  }
  CHECK(differing * 1000 <= actual.rgb.size() / 3);
}

std::array<int, 3> rgb_at(const Rgba8Image& img, int x, int y) {
  const size_t at = ((static_cast<size_t>(y) * static_cast<size_t>(img.width)) + static_cast<size_t>(x)) * 4;
  return {img.rgba.at(at), img.rgba.at(at + 1), img.rgba.at(at + 2)};
}

bool near(const std::array<int, 3>& rgb, int r, int g, int b, int tol) {
  return std::abs(rgb[0] - r) <= tol && std::abs(rgb[1] - g) <= tol && std::abs(rgb[2] - b) <= tol;
}

CompareMode difference(float gain) {
  CompareMode m;
  m.kind = Kind::Difference;
  m.diff_gain = gain;
  return m;
}

}  // namespace

// Side-by-side, vertical split, flicker and the PSNR values are covered on the GPU below; these are the CPU-only cases.
TEST_CASE("render_test_compare: horizontal split, flicker phase and scissor", "[fast][render]") {
  CompareMode m;
  m.kind = Kind::Split;
  m.split_position = 0.25F;
  m.split_axis = CompareMode::Axis::Horizontal;
  auto l = ovt::render::detail::compare_layout(m, 100, 40, false);
  CHECK((l.clip_a.y1 == 10 && l.clip_b.y0 == 10 && l.view_a.y1 == 40 && l.divider.y0 == 9 && l.divider.y1 == 11));
  m.split_position = 1.0F;  // all A, no divider
  l = ovt::render::detail::compare_layout(m, 100, 40, false);
  CHECK((!l.draw_b && l.divider.empty()));

  using ovt::render::detail::flicker_shows_b;
  CHECK((!flicker_shows_b(499, 500) && flicker_shows_b(500, 500) && !flicker_shows_b(1000, 500)));
  CHECK_FALSE(flicker_shows_b(1000, 0));

  pl_rect2df src{0, 0, 64, 32};
  pl_rect2df dst{-64, 0, 64, 64};  // zoom 2
  REQUIRE(ovt::render::detail::clip_rects(src, dst, IRect{0, 0, 30, 64}));
  CHECK((dst.x0 == 0.0F && dst.x1 == 30.0F && src.x0 == 32.0F && src.x1 == 47.0F));
}

TEST_CASE("render_test_compare: slot B conversion and RND-0010/RND-0011 (§10.6.4)", "[fast][render]") {
  using ovt::render::detail::PreparedB;
  using ovt::render::detail::SlotFormat;
  SlotFormat a;
  a.valid = true;
  a.width = kW;
  a.height = kH;
  a.chroma_format = 1;
  a.bit_depth = 10;
  a.sample_bytes = 2;

  OwnedPicture b(kW, kH, 8, 2, 1);  // 8-bit 4:2:2
  b.fill_plane(0, 100);
  b.fill_plane(1, 50);
  b.fill_plane(2, 200);
  PreparedB prep;
  REQUIRE(ovt::render::detail::prepare_b(b.pic, a, false, prep).empty());
  CHECK(prep.converted);
  CHECK(prep.message == "RND-0011 B converted: 4:2:2→4:2:0, 8-bit→10-bit");
  CHECK(prep.pic.color.chroma_format == 1);
  CHECK(prep.pic.planes[1].height == kH / 2);
  CHECK(prep.pic.planes[0].sample_bytes == 2);
  CHECK(prep.storage[0].at(0) == (400 & 0xFF));  // 100 << 2, little-endian
  CHECK(prep.storage[0].at(1) == (400 >> 8));
  CHECK(prep.storage[2].at(0) == (800 & 0xFF));

  OwnedPicture small(32, 16, 10, 1, 2);
  small.fill_plane(0, 500);
  small.fill_plane(1, 512);
  small.fill_plane(2, 512);
  REQUIRE(ovt::render::detail::prepare_b(small.pic, a, false, prep).empty());
  CHECK(prep.size_mismatch);
  CHECK(prep.message == "RND-0010 compare slot B is 32×16, A is 64×32; set allow_scale_b to resample");
  REQUIRE(ovt::render::detail::prepare_b(small.pic, a, true, prep).empty());
  CHECK_FALSE(prep.size_mismatch);
  CHECK(prep.converted);
  CHECK(prep.message.empty());  // a pure resample has no RND line
  CHECK(prep.pic.coded_width == kW);
  CHECK(prep.storage[0].at(static_cast<size_t>((5 * kW) + 7) * 2) == (500 & 0xFF));
}

TEST_CASE("render_test_compare: identical A and B, Difference gain 32, are all zero with PSNR 99 (AT1)",
          "[fast][render][vulkan]") {
  auto s = surface_or_skip(kW, kH);
  const OwnedPicture a = flat10(400, 300, 700);
  const OwnedPicture b = flat10(400, 300, 700, 2);
  s->set_frame(a.pic);
  s->set_compare(b.pic, difference(32.0F));
  CHECK(std::string(s->last_error()).empty());
  const Rgba8Image img = render_image(*s, view_1to1());
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      REQUIRE(rgb_at(img, x, y) == std::array<int, 3>{0, 0, 0});
    }
  }
  const ovt::render::Measurements m = s->measure();
  CHECK(m.region_psnr[0] == 99.0F);
  CHECK(m.region_psnr[1] == 99.0F);
  CHECK(m.region_psnr[2] == 99.0F);
}

TEST_CASE("render_test_compare: B = A + 4 code values on 10-bit luma at gain 8 shows grey 8 (AT2)",
          "[fast][render][vulkan]") {
  auto s = surface_or_skip(kW, kH);
  const OwnedPicture a = flat10(400);
  const OwnedPicture b = flat10(404, 512, 512, 2);
  s->set_frame(a.pic);
  for (const int plane : {-1, 0}) {
    CompareMode m = difference(8.0F);
    m.diff_plane = static_cast<int8_t>(plane);
    s->set_compare(b.pic, m);
    const Rgba8Image img = render_image(*s, view_1to1());
    // round(8 x 4/1023 x 255) = 8, written directly as the output code value
    CHECK(near(rgb_at(img, 0, 0), 8, 8, 8, 1));
    CHECK(near(rgb_at(img, kW - 1, kH - 1), 8, 8, 8, 1));
  }
  // MSE_Y = 16 code values^2 over the visible region; chroma is equal.
  const ovt::render::Measurements m = s->measure();
  CHECK(m.region_psnr[0] == Catch::Approx(10.0 * std::log10(1023.0 * 1023.0 / 16.0)).epsilon(1e-4));
  CHECK(m.region_psnr[1] == 99.0F);

  CompareMode chroma = difference(8.0F);
  chroma.diff_plane = 1;  // U only: equal
  s->set_compare(b.pic, chroma);
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 3, 3), 0, 0, 0, 0));

  CompareMode sgn = difference(8.0F);
  sgn.diff_signed = true;  // 0.5 + 8 x (400 - 404)/1023 / 2 -> 128 - 4
  s->set_compare(b.pic, sgn);
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 3, 3), 124, 124, 124, 1));
  s->set_compare(a.pic, sgn);  // equal: mid-grey
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 3, 3), 128, 128, 128, 1));
}

TEST_CASE("render_test_compare: region PSNR covers only the visible region", "[fast][render][vulkan]") {
  auto s = surface_or_skip(kW, kH);
  const OwnedPicture a = flat10(400);
  OwnedPicture b = flat10(400, 512, 512, 2);
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 48; x < kW; ++x) {
      b.set(0, x, y, 404);
    }
  }
  s->set_frame(a.pic);
  s->set_compare(b.pic, difference(1.0F));
  ViewState v = view_1to1();
  v.zoom = 2.0;
  v.pan_x = 16;  // samples [0, 32) visible: equal
  s->render(v);
  CHECK(s->measure().region_psnr[0] == 99.0F);
  v.pan_x = 48;  // samples [32, 64) x [8, 24): half differ by 4 -> MSE 8
  s->render(v);
  CHECK(s->measure().region_psnr[0] == Catch::Approx(10.0 * std::log10(1023.0 * 1023.0 / 8.0)).epsilon(1e-4));
}

TEST_CASE("render_test_compare: difference golden diff_gain8.png and false colour", "[fast][render][vulkan]") {
  auto s = surface_or_skip(kW, kH);
  OwnedPicture a = flat10(400);
  OwnedPicture b = flat10(400, 512, 512, 2);
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      a.set(0, x, y, 300 + x + (2 * y));
      b.set(0, x, y, 300 + x + (2 * y) + (x / 8) + (y / 8));  // differences 0..10 code values in 8x8 blocks
    }
  }
  s->set_frame(a.pic);
  s->set_compare(b.pic, difference(8.0F));
  const Rgba8Image img = render_image(*s, view_1to1());
  CHECK(render_image(*s, view_1to1()).rgba == img.rgba);  // FR-PLN-101 / §10.9.2 item 5
  CHECK(near(rgb_at(img, 63, 31), 20, 20, 20, 1));        // 10 code values: round(8 x 10/1023 x 255) = 20
  check_golden(img, "diff_gain8.png");

  CompareMode fc = difference(8.0F);
  fc.diff_false_color = true;  // g = 0 -> turbo(0)
  s->set_compare(a.pic, fc);
  const auto& t = ovt::render::luts::kTurbo;
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 5, 5), t[0], t[1], t[2], 1));
}

TEST_CASE("render_test_compare: split, side by side and golden split_v.png", "[fast][render][vulkan]") {
  auto s = surface_or_skip(kW, kH);
  const OwnedPicture a = flat10(64);                // black
  const OwnedPicture b = flat10(940, 512, 512, 2);  // white
  s->set_frame(a.pic);
  CompareMode m;
  m.kind = Kind::Split;
  m.split_position = 0.5F;
  s->set_compare(b.pic, m);
  const Rgba8Image split = render_image(*s, view_1to1());
  CHECK(near(rgb_at(split, 10, 10), 0, 0, 0, 1));
  CHECK(near(rgb_at(split, 50, 10), 255, 255, 255, 1));
  CHECK(near(rgb_at(split, 31, 0), 255, 255, 255, 0));  // divider
  CHECK(near(rgb_at(split, 30, 0), 0, 0, 0, 1));

  // The golden uses coloured pictures so that the conversion of both slots is covered.
  OwnedPicture ca = flat10(300, 400, 600, 3);  // new generations: same-generation uploads are skipped (§10.2.1)
  OwnedPicture cb = flat10(600, 620, 420, 4);
  for (uint32_t x = 0; x < kW; ++x) {
    for (uint32_t y = 0; y < kH; ++y) {
      ca.set(0, x, y, 200 + (8 * x));
      cb.set(0, x, y, 800 - (8 * x));
    }
  }
  s->set_frame(ca.pic);
  s->set_compare(cb.pic, m);
  ViewState v = view_1to1();
  v.zoom = 2.0;  // nearest-neighbour magnification under the scissor
  const Rgba8Image img = render_image(*s, v);
  CHECK(render_image(*s, v).rgba == img.rgba);
  check_golden(img, "split_v.png");

  s->set_frame(a.pic);
  m.kind = Kind::SideBySide;
  s->set_compare(b.pic, m);
  const Rgba8Image sbs = render_image(*s, view_1to1());
  CHECK(near(rgb_at(sbs, 5, 5), 0, 0, 0, 1));
  CHECK(near(rgb_at(sbs, 30, 5), 0, 0, 0, 1));
  CHECK(near(rgb_at(sbs, 31, 5), 255, 255, 255, 0));
  CHECK(near(rgb_at(sbs, 60, 5), 255, 255, 255, 1));
}

TEST_CASE("render_test_compare: overlay blends in linear light, flicker follows the clock", "[fast][render][vulkan]") {
  auto s = surface_or_skip(kW, kH);
  const OwnedPicture a = flat10(64);
  const OwnedPicture b = flat10(940, 512, 512, 2);
  s->set_frame(a.pic);
  CompareMode m;
  m.kind = Kind::Overlay;
  m.overlay_alpha = 0.5F;
  s->set_compare(b.pic, m);
  // (1 - 0.5) x 0 + 0.5 x 1.0 linear -> sRGB 0.735 -> 188
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 7, 7), 188, 188, 188, 2));

  int64_t now = 1000;
  ovt::render::detail::set_compare_clock(*s, [&now] { return now; });
  m.kind = Kind::Flicker;
  m.flicker_period_ms = 500;
  s->set_compare(b.pic, m);
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 7, 7), 0, 0, 0, 1));
  now = 1600;
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 7, 7), 255, 255, 255, 1));
  now = 2100;
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 7, 7), 0, 0, 0, 1));

  s->set_compare(b.pic, CompareMode{});  // Kind::Single clears B
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 7, 7), 0, 0, 0, 1));
}

TEST_CASE("render_test_compare: mismatched B: hatch (RND-0010), resample and conversion (RND-0011)",
          "[fast][render][vulkan]") {
  auto s = surface_or_skip(kW, kH);
  const OwnedPicture a = flat10(500);
  s->set_frame(a.pic);
  OwnedPicture small(32, 16, 10, 1, 2);
  small.fill_plane(0, 500);
  small.fill_plane(1, 512);
  small.fill_plane(2, 512);
  small.pic.generation = 2;
  s->set_compare(small.pic, difference(32.0F));
  CHECK(std::string(s->last_error()).rfind("RND-0010 compare slot B is 32×16, A is 64×32", 0) == 0);
  const Rgba8Image hatch = render_image(*s, view_1to1());
  bool dark = false;
  bool light = false;
  for (int x = 0; x < 16; ++x) {
    dark = dark || near(rgb_at(hatch, x, 0), 64, 64, 64, 1);
    light = light || near(rgb_at(hatch, x, 0), 128, 128, 128, 1);
  }
  CHECK(dark);
  CHECK(light);
  CHECK(s->measure().region_psnr[0] == -1.0F);

  CompareMode scale = difference(32.0F);
  scale.allow_scale_b = true;
  s->set_compare(small.pic, scale);
  CHECK(std::string(s->last_error()).empty());
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 9, 9), 0, 0, 0, 0));
  CHECK(s->measure().region_psnr[0] == 99.0F);

  OwnedPicture b8(kW, kH, 8, 2, 1);  // 8-bit 4:2:2, code 125 << 2 = 500
  b8.fill_plane(0, 125);
  b8.fill_plane(1, 128);
  b8.fill_plane(2, 128);
  b8.pic.generation = 3;
  s->set_compare(b8.pic, difference(32.0F));
  CHECK(std::string(s->last_error()) == "RND-0011 B converted: 4:2:2→4:2:0, 8-bit→10-bit");
  CHECK(near(rgb_at(render_image(*s, view_1to1()), 9, 9), 0, 0, 0, 0));
}
