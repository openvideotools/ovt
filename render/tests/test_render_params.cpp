// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.403 (chapter 10 §10.11): render() maps the source to the four output encodings with the ViewState options:
// §10.3.2 target frames, §10.3.4 tone and gamut mapping (R-TM-1), §10.3.5 dithering, and deterministic output
// (FR-PLN-101).

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <libplacebo/filters.h>
#include <libplacebo/gamut_mapping.h>
#include <libplacebo/renderer.h>
#include <libplacebo/tone_mapping.h>

#include "ovt/render/video_surface.h"
#include "pipeline/render_params.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::DisplayTarget;
using ovt::render::GamutMap;
using ovt::render::ToneMap;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::OutputPixels;
using ovt::render::detail::RenderParamSet;
using ovt::render::test::OwnedPicture;
using ovt::render::test::surface_or_skip;
using Encoding = DisplayTarget::OutputEncoding;

DisplayTarget offscreen(DisplayTarget::OffscreenFormat fmt) {
  DisplayTarget t;
  t.width = 16;
  t.height = 16;
  t.offscreen_format = fmt;
  return t;
}

// libplacebo objects compared by name: with a static libplacebo, libovt-render and this test hold separate copies, so
// their addresses differ even when the object is the same.
template <typename T>
std::string name_of(const T* obj) {
  return obj != nullptr && obj->name != nullptr ? obj->name : "";
}

// The target frame build() starts from: §10.3.2 colour of the encoding.
struct pl_frame target_frame(const DisplayTarget& t, Encoding e) {
  struct pl_frame f = {};
  f.color = ovt::render::detail::target_color(t, e);
  return f;
}

struct pl_color_space pq_source() {
  struct pl_color_space c = pl_color_space_unknown;
  c.primaries = PL_COLOR_PRIM_BT_2020;
  c.transfer = PL_COLOR_TRC_PQ;
  return c;
}

// tests/corpora/patterns/pq_patches_1000.expected.json (WI-1.413): six BT.2020 PQ limited-range grey patches.
nlohmann::json pq_patches_expected() {
  std::ifstream in(std::string(OVT_PATTERNS_DIR) + "/pq_patches_1000.expected.json");
  REQUIRE(in.good());
  return nlohmann::json::parse(in);
}

// The pattern's frame scaled down 20x (1920x1080 -> 96x54 is not 4:2:0 even, so 96x8): the patch columns of the
// expected.json divided by 20, each patch filled with its limited-range code, chroma neutral.
constexpr uint32_t kScale = 20;
constexpr uint32_t kPatchesH = 8;

OwnedPicture pq_patches(const nlohmann::json& exp) {
  const auto width = exp.at("width").get<uint32_t>() / kScale;
  OwnedPicture op(width, kPatchesH, exp.at("bit_depth").get<uint8_t>(), 1, 2);
  op.pic.color.primaries = 9;  // BT.2020
  op.pic.color.transfer = 16;  // PQ
  op.pic.color.matrix = 9;     // BT.2020 NCL
  const auto& columns = exp.at("patch_columns");
  const auto& codes = exp.at("codes_limited");
  for (size_t p = 0; p < columns.size(); ++p) {
    const uint32_t x0 = columns[p][0].get<uint32_t>() / kScale;
    const uint32_t x1 = columns[p][1].get<uint32_t>() / kScale;
    for (uint32_t y = 0; y < kPatchesH; ++y) {
      for (uint32_t x = x0; x < x1; ++x) {
        op.set(0, x, y, codes[p].get<uint32_t>());
      }
    }
  }
  op.fill_plane(1, 512);
  op.fill_plane(2, 512);
  op.pic.generation = 1;
  return op;
}

ViewState one_to_one(double w, double h) {
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = w / 2.0;
  view.pan_y = h / 2.0;
  return view;
}

OutputPixels render_and_download(VideoSurface& surface, const ViewState& view) {
  surface.render(view);
  REQUIRE(std::string(surface.last_error()).empty());
  OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(surface, px));
  return px;
}

// The RGB bytes of every pixel of patch p (columns of the scaled frame), row-major.
std::vector<uint8_t> patch_rgb(const OutputPixels& px, const nlohmann::json& exp, size_t p) {
  const uint32_t x0 = exp.at("patch_columns")[p][0].get<uint32_t>() / kScale;
  const uint32_t x1 = exp.at("patch_columns")[p][1].get<uint32_t>() / kScale;
  std::vector<uint8_t> out;
  for (uint32_t y = 0; y < kPatchesH; ++y) {
    for (uint32_t x = x0; x < x1; ++x) {
      const size_t i = ((size_t{y} * static_cast<size_t>(px.width)) + x) * 4;
      out.insert(out.end(), px.bytes.begin() + static_cast<std::ptrdiff_t>(i),
                 px.bytes.begin() + static_cast<std::ptrdiff_t>(i + 3));
    }
  }
  return out;
}

// IEEE 754 binary16 (little-endian, as Rgba16F output is downloaded) to float.
float half_at(const std::vector<uint8_t>& bytes, size_t at) {
  const auto h = static_cast<uint16_t>(bytes.at(at) | (bytes.at(at + 1) << 8U));
  const float sign = (h & 0x8000U) != 0 ? -1.0F : 1.0F;
  const auto exponent = static_cast<int>((h >> 10U) & 0x1FU);
  const auto mantissa = static_cast<float>(h & 0x3FFU);
  if (exponent == 0) {
    return sign * std::ldexp(mantissa, -24);  // subnormal
  }
  if (exponent == 31) {
    return mantissa == 0.0F ? sign * INFINITY : NAN;
  }
  return sign * std::ldexp(1024.0F + mantissa, exponent - 25);
}

}  // namespace

TEST_CASE("render_test_render_params: target frame per encoding (§10.3.2)", "[fast][render]") {
  using ovt::render::detail::is_float_encoding;
  using ovt::render::detail::target_color;
  using ovt::render::detail::target_color_depth;
  using ovt::render::detail::target_encoding;

  CHECK(target_encoding(offscreen(DisplayTarget::OffscreenFormat::Rgba8Srgb)) == Encoding::Srgb8);
  CHECK(target_encoding(offscreen(DisplayTarget::OffscreenFormat::Rgba16F)) == Encoding::ScRgbLinear16F);
  CHECK(target_encoding(offscreen(DisplayTarget::OffscreenFormat::Rgb10A2)) == Encoding::Pq10);
  DisplayTarget native = offscreen(DisplayTarget::OffscreenFormat::Rgba8Srgb);
  native.kind = DisplayTarget::Kind::MetalLayer;
  native.encoding = Encoding::Hlg10;
  CHECK(target_encoding(native) == Encoding::Hlg10);

  DisplayTarget t = offscreen(DisplayTarget::OffscreenFormat::Rgba8Srgb);
  t.headroom = 4.0F;
  t.sdr_white_nits = 250.0F;
  const auto srgb = target_color(t, Encoding::Srgb8);
  CHECK(srgb.primaries == PL_COLOR_PRIM_BT_709);
  CHECK(srgb.transfer == PL_COLOR_TRC_SRGB);
  CHECK(srgb.hdr.max_luma == PL_COLOR_SDR_WHITE);
  const auto lin = target_color(t, Encoding::ScRgbLinear16F);
  CHECK(lin.primaries == PL_COLOR_PRIM_BT_709);
  CHECK(lin.transfer == PL_COLOR_TRC_LINEAR);
  CHECK(lin.hdr.max_luma == 1000.0F);  // headroom x sdr_white
  const auto pq = target_color(t, Encoding::Pq10);
  CHECK(pq.primaries == PL_COLOR_PRIM_BT_2020);
  CHECK(pq.transfer == PL_COLOR_TRC_PQ);
  CHECK(pq.hdr.max_luma == 1000.0F);  // display max
  t.headroom = 1.0F;                  // display max unknown -> 1000
  CHECK(target_color(t, Encoding::Pq10).hdr.max_luma == 1000.0F);
  t.headroom = 8.0F;
  t.sdr_white_nits = 203.0F;
  CHECK(target_color(t, Encoding::Pq10).hdr.max_luma == 1624.0F);
  const auto hlg = target_color(t, Encoding::Hlg10);
  CHECK(hlg.primaries == PL_COLOR_PRIM_BT_2020);
  CHECK(hlg.transfer == PL_COLOR_TRC_HLG);
  CHECK(hlg.hdr.max_luma == 1000.0F);

  CHECK(target_color_depth(Encoding::Srgb8) == 8);
  CHECK(target_color_depth(Encoding::ScRgbLinear16F) == 16);
  CHECK(target_color_depth(Encoding::Pq10) == 10);
  CHECK(target_color_depth(Encoding::Hlg10) == 10);
  CHECK(is_float_encoding(Encoding::ScRgbLinear16F));
  CHECK_FALSE(is_float_encoding(Encoding::Srgb8));
}

TEST_CASE("render_test_render_params: tone mapping table and R-TM-1 (§10.3.4)", "[fast][render]") {
  const DisplayTarget t = offscreen(DisplayTarget::OffscreenFormat::Rgba8Srgb);
  RenderParamSet set;
  ViewState view;

  struct Row {
    ToneMap tm;
    const struct pl_tone_map_function* fn;
  };
  for (const Row& row : {Row{ToneMap::Auto, &pl_tone_map_spline}, Row{ToneMap::BT2390, &pl_tone_map_bt2390},
                         Row{ToneMap::BT2446A, &pl_tone_map_bt2446a}, Row{ToneMap::Spline, &pl_tone_map_spline},
                         Row{ToneMap::Clip, &pl_tone_map_clip}, Row{ToneMap::None, &pl_tone_map_clip}}) {
    INFO("tone_map " << static_cast<int>(row.tm));
    view.tone_map = row.tm;
    struct pl_frame target = target_frame(t, Encoding::Srgb8);
    set.build(view, Encoding::Srgb8, /*offscreen=*/false, pq_source(), target);
    REQUIRE(set.params().color_map_params != nullptr);
    CHECK(name_of(set.params().color_map_params->tone_mapping_function) == name_of(row.fn));
    CHECK(set.params().color_map_params->metadata == PL_HDR_METADATA_ANY);
    // Peak detection only for Auto on a live display.
    CHECK((set.params().peak_detect_params != nullptr) == (row.tm == ToneMap::Auto));
    CHECK(set.params().color_map_params->contrast_recovery == (row.tm == ToneMap::Auto ? 0.3F : 0.0F));

    // Offscreen, export and test renders never use peak detection (FR-PLN-101).
    target = target_frame(t, Encoding::Srgb8);
    set.build(view, Encoding::Srgb8, /*offscreen=*/true, pq_source(), target);
    CHECK(set.params().peak_detect_params == nullptr);
  }

  SECTION("Clip sets the target peak to clip_nits") {
    view.tone_map = ToneMap::Clip;
    view.clip_nits = 500.0F;
    struct pl_frame target = target_frame(t, Encoding::Srgb8);
    set.build(view, Encoding::Srgb8, true, pq_source(), target);
    CHECK(target.color.hdr.max_luma == 500.0F);
  }
  SECTION("None passes the source peak through") {
    view.tone_map = ToneMap::None;
    struct pl_frame target = target_frame(t, Encoding::Srgb8);
    struct pl_color_space src = pq_source();
    set.build(view, Encoding::Srgb8, true, src, target);
    CHECK(target.color.hdr.max_luma == 10000.0F);  // PQ without mastering metadata
    src.hdr.max_luma = 4000.0F;
    target = target_frame(t, Encoding::Srgb8);
    set.build(view, Encoding::Srgb8, true, src, target);
    CHECK(target.color.hdr.max_luma == 4000.0F);
  }
  SECTION("target_peak_override replaces the display peak") {
    view.tone_map = ToneMap::BT2390;
    view.target_peak_override = 600.0F;
    struct pl_frame target = target_frame(t, Encoding::Pq10);
    set.build(view, Encoding::Pq10, true, pq_source(), target);
    CHECK(target.color.hdr.max_luma == 600.0F);
  }
}

TEST_CASE("render_test_render_params: gamut mapping per ViewState (§10.3.4)", "[fast][render]") {
  const DisplayTarget t = offscreen(DisplayTarget::OffscreenFormat::Rgba8Srgb);
  RenderParamSet set;
  ViewState view;
  struct Row {
    GamutMap gm;
    const struct pl_gamut_map_function* fn;
  };
  for (const Row& row :
       {Row{GamutMap::Perceptual, &pl_gamut_map_perceptual}, Row{GamutMap::Relative, &pl_gamut_map_relative},
        Row{GamutMap::Saturation, &pl_gamut_map_saturation}, Row{GamutMap::Absolute, &pl_gamut_map_absolute},
        Row{GamutMap::Desaturate, &pl_gamut_map_desaturate}, Row{GamutMap::Darken, &pl_gamut_map_darken},
        Row{GamutMap::Clip, &pl_gamut_map_clip}, Row{GamutMap::Highlight, &pl_gamut_map_highlight}}) {
    view.gamut_map = row.gm;
    struct pl_frame target = target_frame(t, Encoding::Srgb8);
    set.build(view, Encoding::Srgb8, true, pq_source(), target);
    CHECK(name_of(set.params().color_map_params->gamut_mapping) == name_of(row.fn));
  }
  // §10.7.8: the gamut clip overlay is pl_gamut_map_highlight whatever gamut_map says.
  view.gamut_map = GamutMap::Perceptual;
  view.gamut_clip_overlay = true;
  struct pl_frame target = target_frame(t, Encoding::Srgb8);
  set.build(view, Encoding::Srgb8, true, pq_source(), target);
  CHECK(name_of(set.params().color_map_params->gamut_mapping) == name_of(&pl_gamut_map_highlight));
}

TEST_CASE("render_test_render_params: dithering and filters (§10.3.5, §10.3.1)", "[fast][render]") {
  const DisplayTarget t = offscreen(DisplayTarget::OffscreenFormat::Rgba8Srgb);
  RenderParamSet set;
  ViewState view;
  for (const Encoding e : {Encoding::Srgb8, Encoding::Pq10, Encoding::Hlg10}) {
    struct pl_frame target = target_frame(t, e);
    set.build(view, e, true, pq_source(), target);
    REQUIRE(set.params().dither_params != nullptr);
    CHECK(set.params().dither_params->method == PL_DITHER_BLUE_NOISE);
    CHECK(set.params().dither_params->lut_size == 6);
    CHECK_FALSE(set.params().dither_params->temporal);
    CHECK(target.repr.bits.color_depth == ovt::render::detail::target_color_depth(e));
  }
  struct pl_frame target = target_frame(t, Encoding::ScRgbLinear16F);
  set.build(view, Encoding::ScRgbLinear16F, true, pq_source(), target);
  CHECK(set.params().dither_params == nullptr);  // float target: libplacebo asserts otherwise
  // libplacebo infers sample_depth only for UNORM targets; a float target needs it equal to color_depth, or the
  // output is scaled by 257 (see the Rgba16F read-back test below).
  CHECK(target.repr.bits.sample_depth == target.repr.bits.color_depth);
  view.dither = false;
  target = target_frame(t, Encoding::Srgb8);
  set.build(view, Encoding::Srgb8, true, pq_source(), target);
  CHECK(set.params().dither_params == nullptr);  // banding shown deliberately

  view.zoom = 4.0;
  set.build(view, Encoding::Srgb8, true, pq_source(), target);
  CHECK(name_of(set.params().upscaler) == name_of(&pl_filter_nearest));
  CHECK(set.params().skip_anti_aliasing);
  view.zoom = 0.5;
  view.downscale = ovt::render::Downscale::Lanczos;
  view.chroma_upscale = ovt::render::ChromaUpscale::Nearest;
  set.build(view, Encoding::Srgb8, true, pq_source(), target);
  CHECK(name_of(set.params().downscaler) == name_of(&pl_filter_lanczos));
  CHECK(name_of(set.params().plane_upscaler) == name_of(&pl_filter_nearest));
  CHECK_FALSE(set.params().skip_anti_aliasing);

  // §10.2.3 viewport mapping: device = (image - pan) * zoom + viewport / 2.
  ViewState v2;
  v2.zoom = 2.0;
  v2.pan_x = 10;
  v2.pan_y = 5;
  const pl_rect2df r = ovt::render::detail::target_rect(pl_rect2df{0, 0, 20, 10}, v2, 100, 50);
  CHECK(r.x0 == 30.0F);
  CHECK(r.y0 == 15.0F);
  CHECK(r.x1 == 70.0F);
  CHECK(r.y1 == 35.0F);
}

// Acceptance test 1.
TEST_CASE("render_test_render_params: Clip at 500 nits renders the 1000 and 4000 nit patches identically",
          "[fast][render][vulkan]") {
  const nlohmann::json exp = pq_patches_expected();
  REQUIRE(exp.at("nits")[3].get<double>() == 1000.0);
  REQUIRE(exp.at("nits")[4].get<double>() == 4000.0);
  const OwnedPicture op = pq_patches(exp);
  const auto w = static_cast<int>(op.pic.coded_width);
  auto surface = surface_or_skip(w, static_cast<int>(kPatchesH));
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  ViewState view = one_to_one(w, kPatchesH);
  view.tone_map = ToneMap::Clip;
  view.clip_nits = 500.0F;
  // Blue-noise dither adds a position-dependent offset below one code, and the two patches sit at different
  // positions: a value a fraction of a code under the peak (pl_gamut_map_perceptual leaves the 4000-nit grey there)
  // dithers to 254 at some positions and 255 at others. The tone curve is what this test checks, so it renders
  // undithered and the dithered render is held to one code below.
  view.dither = false;
  const OutputPixels px = render_and_download(*surface, view);
  const std::vector<uint8_t> p1000 = patch_rgb(px, exp, 3);
  const std::vector<uint8_t> p4000 = patch_rgb(px, exp, 4);
  CHECK(p1000 == p4000);
  // Both are above clip_nits, so they sit at the display peak; the 100-nit patch stays below it.
  CHECK(p1000.front() == 255);
  CHECK(patch_rgb(px, exp, 1).front() < 255);

  SECTION("dithered, the two patches stay within one code of the peak") {
    view.dither = true;
    const OutputPixels dithered = render_and_download(*surface, view);
    for (const size_t p : {size_t{3}, size_t{4}}) {
      for (const uint8_t v : patch_rgb(dithered, exp, p)) {
        CHECK(v >= 254);
      }
    }
  }
  SECTION("BT2390 keeps them apart") {
    view.tone_map = ToneMap::BT2390;
    view.clip_nits = 1000.0F;
    const OutputPixels mapped = render_and_download(*surface, view);
    CHECK(patch_rgb(mapped, exp, 3) != patch_rgb(mapped, exp, 4));
  }
}

// Acceptance test 2 (and FR-PLN-101's Rgba16F form): the same picture and ViewState give byte-identical output, even
// with a different picture rendered in between (no frame history: R-TM-1).
TEST_CASE("render_test_render_params: rendering twice is byte-identical", "[fast][render][vulkan]") {
  const nlohmann::json exp = pq_patches_expected();
  const OwnedPicture a = pq_patches(exp);
  OwnedPicture b = pq_patches(exp);
  b.fill_plane(0, 940);  // all 10000 nits: would move a scene-adaptive peak
  b.pic.generation = 2;
  const auto w = static_cast<int>(a.pic.coded_width);
  for (const auto fmt : {DisplayTarget::OffscreenFormat::Rgba8Srgb, DisplayTarget::OffscreenFormat::Rgba16F,
                         DisplayTarget::OffscreenFormat::Rgb10A2}) {
    INFO("offscreen format " << static_cast<int>(fmt));
    auto surface = surface_or_skip(w, static_cast<int>(kPatchesH), fmt);
    for (const ToneMap tm : {ToneMap::Auto, ToneMap::BT2390, ToneMap::Spline}) {
      INFO("tone_map " << static_cast<int>(tm));
      ViewState view = one_to_one(w, kPatchesH);
      view.tone_map = tm;
      surface->set_frame(a.pic);
      const OutputPixels first = render_and_download(*surface, view);
      surface->set_frame(b.pic);
      static_cast<void>(render_and_download(*surface, view));
      surface->set_frame(a.pic);
      const OutputPixels second = render_and_download(*surface, view);
      REQUIRE(first.bytes.size() == second.bytes.size());
      CHECK(first.bytes == second.bytes);
    }
  }
}

// Regression: a float target with repr.bits.sample_depth left 0 rendered limited-range 10-bit white (code 940) as
// 257.0 and alpha as 257.0, because libplacebo defaults an unset sample_depth to 8 bits and scales by (2^16-1)/(2^8-1).
// SDR reference white is 1.0 in the scRGB-linear target (§10.3.2), and the output alpha is opaque.
TEST_CASE("render_test_render_params: SDR white and alpha read back 1.0 on an Rgba16F target",
          "[fast][render][vulkan]") {
  constexpr uint32_t kSize = 16;
  OwnedPicture op(kSize, kSize, 10, 1, 2);  // BT.709 limited range, 4:2:0
  op.fill_plane(0, 940);
  op.fill_plane(1, 512);
  op.fill_plane(2, 512);
  op.pic.generation = 1;
  auto surface = surface_or_skip(kSize, kSize, DisplayTarget::OffscreenFormat::Rgba16F);
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  const OutputPixels px = render_and_download(*surface, one_to_one(kSize, kSize));
  REQUIRE(px.texel_bytes == 8);
  REQUIRE(px.bytes.size() == size_t{kSize} * kSize * 8);
  constexpr float kTolerance = 0.01F;  // half float steps by 2^-10 just below 1.0
  for (size_t i = 0; i < px.bytes.size(); i += 8) {
    INFO("pixel " << i / 8);
    for (size_t c = 0; c < 4; ++c) {
      INFO("channel " << c);
      const float v = half_at(px.bytes, i + (c * 2));
      CHECK(std::fabs(v - 1.0F) <= kTolerance);
    }
  }
}
