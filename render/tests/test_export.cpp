// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.412 (chapter 10 §10.8): VideoSurface::export_png() for HdrPng16, SdrPng8 and ViewPng8. The CPU half (colour
// targets, cICP/mDCV/cLLi chunks, sample packing, lodepng encode) is tested directly; the GPU half renders on the
// offscreen surface (lavapipe on Linux CI, MoltenVK on macOS) and re-reads the written PNG with lodepng. The pattern
// pictures are synthesised from tests/corpora/patterns/*.expected.json (WI-1.413), as in test_measure.cpp.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <lodepng.h>

#include "export/png_export.h"
#include "ovt/render/video_surface.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::ExportKind;
using ovt::render::ToneMap;
using ovt::render::ViewState;
using ovt::render::test::OwnedPicture;
using ovt::render::test::surface_or_skip;
namespace detail = ovt::render::detail;

nlohmann::json expected(const std::string& pattern) {
  std::ifstream in(std::string(OVT_PATTERNS_DIR) + "/" + pattern + ".expected.json");
  REQUIRE(in.good());
  return nlohmann::json::parse(in);
}

std::string temp_png(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("ovt_test_export_" + name + ".png")).string();
}

std::vector<uint8_t> read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

uint32_t be32(const std::vector<uint8_t>& b, size_t at) {
  return (uint32_t{b.at(at)} << 24U) | (uint32_t{b.at(at + 1)} << 16U) | (uint32_t{b.at(at + 2)} << 8U) |
         uint32_t{b.at(at + 3)};
}

struct Chunk {
  std::string type;
  std::vector<uint8_t> data;
};

// The chunk sequence of a PNG file, in file order.
std::vector<Chunk> chunks(const std::vector<uint8_t>& png) {
  static const std::vector<uint8_t> kSig = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  REQUIRE(png.size() >= 8);
  REQUIRE(std::vector<uint8_t>(png.begin(), png.begin() + 8) == kSig);
  std::vector<Chunk> out;
  size_t at = 8;
  while (at + 12 <= png.size()) {
    const uint32_t len = be32(png, at);
    REQUIRE(at + 12 + len <= png.size());
    Chunk c;
    c.type.assign(png.begin() + static_cast<std::ptrdiff_t>(at + 4), png.begin() + static_cast<std::ptrdiff_t>(at + 8));
    c.data.assign(png.begin() + static_cast<std::ptrdiff_t>(at + 8),
                  png.begin() + static_cast<std::ptrdiff_t>(at + 8 + len));
    out.push_back(std::move(c));
    at += 12 + len;
  }
  return out;
}

// Index of the first chunk of `type`, or -1.
int find_chunk(const std::vector<Chunk>& cs, const std::string& type) {
  for (size_t i = 0; i < cs.size(); ++i) {
    if (cs[i].type == type) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

struct Decoded {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<uint8_t> samples;  // as lodepng returns them: 16-bit samples big-endian
};

Decoded decode(const std::vector<uint8_t>& png, LodePNGColorType type, unsigned depth) {
  unsigned char* out = nullptr;
  Decoded d;
  const unsigned err = lodepng_decode_memory(&out, &d.width, &d.height, png.data(), png.size(), type, depth);
  const std::unique_ptr<unsigned char, void (*)(void*)> owned(out, &std::free);
  INFO("lodepng: " << lodepng_error_text(err));
  REQUIRE(err == 0);
  const size_t n = size_t{d.width} * d.height * (type == LCT_RGBA ? 4U : 3U) * (depth / 8U);
  d.samples.assign(out, out + n);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return d;
}

uint16_t rgb16(const Decoded& d, unsigned x, unsigned y, unsigned c) {
  const size_t at = ((size_t{y} * d.width + x) * 3 + c) * 2;
  return static_cast<uint16_t>((d.samples.at(at) << 8U) | d.samples.at(at + 1));
}

// 10-bit limited BT.2020 4:2:0 picture of w x h, luma `fill`, neutral chroma.
OwnedPicture bt2020_picture(uint32_t w, uint32_t h, uint8_t transfer, uint32_t fill) {
  OwnedPicture op(w, h, 10, 1, 2);
  op.pic.color.primaries = 9;
  op.pic.color.transfer = transfer;
  op.pic.color.matrix = 9;
  op.fill_plane(0, fill);
  op.fill_plane(1, 512);
  op.fill_plane(2, 512);
  return op;
}

// tests/corpora/patterns/pq_patches_1000 frame 0: six equal-width full-height columns at the limited codes.
OwnedPicture pq_patches(const nlohmann::json& exp) {
  OwnedPicture op = bt2020_picture(exp.at("width").get<uint32_t>(), exp.at("height").get<uint32_t>(), 16, 64);
  const auto& columns = exp.at("patch_columns");
  const auto& codes = exp.at("codes_limited");
  for (size_t p = 0; p < columns.size(); ++p) {
    for (uint32_t y = 0; y < op.pic.coded_height; ++y) {
      for (uint32_t x = columns[p][0].get<uint32_t>(); x < columns[p][1].get<uint32_t>(); ++x) {
        op.set(0, x, y, codes[p].get<uint32_t>());
      }
    }
  }
  return op;
}

// 8-bit limited BT.709 4:4:4 ramp of w x h with some colour, for the SDR and view exports.
OwnedPicture sdr_ramp(uint32_t w, uint32_t h) {
  OwnedPicture op(w, h, 8, 3, 1);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      op.set(0, x, y, 16 + ((x * 219) / (w - 1)));
      op.set(1, x, y, 128 + ((y * 60) / (h - 1)) - 30);
      op.set(2, x, y, 98 + ((x * 60) / (w - 1)));
    }
  }
  return op;
}

ViewState one_to_one(const OwnedPicture& op) {
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = op.pic.coded_width / 2.0;
  view.pan_y = op.pic.coded_height / 2.0;
  view.dither = false;
  return view;
}

// PQ inverse EOTF (ST 2084), the reference of the AT1 tolerance.
double pq_oetf(double nits) {
  const double m1 = 2610.0 / 16384.0;
  const double m2 = 2523.0 / 4096.0 * 128.0;
  const double c1 = 3424.0 / 4096.0;
  const double c2 = 2413.0 / 4096.0 * 32.0;
  const double c3 = 2392.0 / 4096.0 * 32.0;
  const double y = std::pow(nits / 10000.0, m1);
  return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

}  // namespace

// ---- CPU half -------------------------------------------------------------------------------------------------------

TEST_CASE("render_test_export: §10.8 colour target and cICP per kind and source", "[fast][render][export]") {
  struct pl_color_space pq = {};
  pq.primaries = PL_COLOR_PRIM_BT_2020;
  pq.transfer = PL_COLOR_TRC_PQ;
  detail::ExportTarget t = detail::export_target(ExportKind::HdrPng16, pq);
  CHECK(t.bit_depth == 16);
  CHECK(t.hdr_pass);
  CHECK(t.color.primaries == PL_COLOR_PRIM_BT_2020);
  CHECK(t.color.transfer == PL_COLOR_TRC_PQ);
  CHECK((t.cicp.primaries == 9 && t.cicp.transfer == 16 && t.cicp.matrix == 0 && t.cicp.full_range == 1));

  struct pl_color_space hlg = pq;
  hlg.transfer = PL_COLOR_TRC_HLG;
  t = detail::export_target(ExportKind::HdrPng16, hlg);
  CHECK(t.color.transfer == PL_COLOR_TRC_HLG);  // the data stays HLG-encoded (§10.8)
  CHECK((t.cicp.primaries == 9 && t.cicp.transfer == 18 && t.cicp.matrix == 0 && t.cicp.full_range == 1));

  struct pl_color_space sdr = {};
  sdr.primaries = PL_COLOR_PRIM_BT_709;
  sdr.transfer = PL_COLOR_TRC_BT_1886;
  t = detail::export_target(ExportKind::HdrPng16, sdr);
  CHECK(t.bit_depth == 16);  // SDR sources exported as HdrPng16 are 16-bit sRGB
  CHECK_FALSE(t.hdr_pass);
  CHECK(t.color.transfer == PL_COLOR_TRC_SRGB);
  CHECK((t.cicp.primaries == 1 && t.cicp.transfer == 13 && t.cicp.matrix == 0 && t.cicp.full_range == 1));

  t = detail::export_target(ExportKind::SdrPng8, pq);
  CHECK(t.bit_depth == 8);
  CHECK_FALSE(t.hdr_pass);
  CHECK(t.color.primaries == PL_COLOR_PRIM_BT_709);
  CHECK(t.color.transfer == PL_COLOR_TRC_SRGB);
  CHECK((t.cicp.primaries == 1 && t.cicp.transfer == 13 && t.cicp.matrix == 0 && t.cicp.full_range == 1));
}

TEST_CASE("render_test_export: mDCV and cLLi from the source's static HDR metadata", "[fast][render][export]") {
  struct pl_hdr_metadata hdr = {};
  CHECK_FALSE(detail::hdr_chunks(hdr).has_mdcv);
  CHECK_FALSE(detail::hdr_chunks(hdr).has_clli);

  hdr.prim = *pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
  hdr.min_luma = 0.005F;
  hdr.max_luma = 1000.0F;
  hdr.max_cll = 1000.0F;
  hdr.max_fall = 400.0F;
  const detail::PngHdrChunks c = detail::hdr_chunks(hdr);
  REQUIRE(c.has_mdcv);
  CHECK(c.red_x == 35400);  // 0.708 / 0.00002
  CHECK(c.red_y == 14600);
  CHECK(c.green_x == 8500);
  CHECK(c.green_y == 39850);
  CHECK(c.blue_x == 6550);
  CHECK(c.blue_y == 2300);
  CHECK(c.white_x == 15635);  // D65 0.3127
  CHECK(c.white_y == 16450);
  CHECK(c.max_luminance == 10000000);
  CHECK(c.min_luminance == 50);
  REQUIRE(c.has_clli);
  CHECK(c.max_cll == 10000000);
  CHECK(c.max_fall == 4000000);

  hdr.max_luma = 0.0F;  // primaries without a mastering peak: no mDCV
  CHECK_FALSE(detail::hdr_chunks(hdr).has_mdcv);
}

TEST_CASE("render_test_export: lodepng encode keeps type and depth, chunks before IDAT, same bytes twice",
          "[fast][render][export]") {
  // 3 x 2 RGB, 16-bit: host-order RGBA16 texels in, big-endian RGB out.
  std::vector<uint8_t> texels(size_t{3} * 2 * 8);
  for (size_t i = 0; i < size_t{3} * 2 * 4; ++i) {
    const auto v = static_cast<uint16_t>((i * 4099U) & 0xFFFFU);
    std::memcpy(&texels[i * 2], &v, 2);
  }
  detail::PngImage img;
  REQUIRE(detail::pack_samples(texels, 8, 3, 2, 3, img));
  CHECK(img.bit_depth == 16);
  CHECK(img.samples.size() == size_t{3} * 2 * 3 * 2);
  img.has_cicp = true;
  img.cicp = detail::PngCicp{9, 16, 0, 1};
  img.hdr.has_mdcv = true;
  img.hdr.red_x = 35400;
  img.hdr.max_luminance = 10000000;
  img.hdr.has_clli = true;
  img.hdr.max_cll = 10000000;
  img.hdr.max_fall = 4000000;

  std::vector<uint8_t> png;
  std::string err;
  REQUIRE(detail::encode_png(img, png, err));
  std::vector<uint8_t> again;
  REQUIRE(detail::encode_png(img, again, err));
  CHECK(png == again);  // §10.9.2 item 5: same input, same bytes

  const std::vector<Chunk> cs = chunks(png);
  REQUIRE(find_chunk(cs, "IHDR") == 0);
  CHECK(cs[0].data.at(8) == 16);  // bit depth
  CHECK(cs[0].data.at(9) == 2);   // colour type 2 (RGB), not converted
  const int cicp = find_chunk(cs, "cICP");
  const int mdcv = find_chunk(cs, "mDCV");
  const int clli = find_chunk(cs, "cLLI");
  const int idat = find_chunk(cs, "IDAT");
  REQUIRE(cicp > 0);
  REQUIRE(mdcv > 0);
  REQUIRE(clli > 0);
  REQUIRE(idat > 0);
  CHECK(cicp < idat);
  CHECK(mdcv < idat);
  CHECK(clli < idat);
  CHECK(cs[static_cast<size_t>(cicp)].data == std::vector<uint8_t>{9, 16, 0, 1});
  CHECK(be32(cs[static_cast<size_t>(clli)].data, 0) == 10000000);
  CHECK(be32(cs[static_cast<size_t>(clli)].data, 4) == 4000000);
  CHECK(find_chunk(cs, "tEXt") < 0);

  const Decoded d = decode(png, LCT_RGB, 16);
  CHECK(d.width == 3);
  CHECK(d.height == 2);
  CHECK(d.samples == img.samples);

  // 8-bit RGBA keeps its alpha and colour type 6.
  const std::vector<uint8_t> rgba = {10, 20, 30, 0, 40, 50, 60, 255};
  CHECK(detail::has_transparency(rgba));
  CHECK_FALSE(detail::has_transparency({1, 2, 3, 255}));
  detail::PngImage img8;
  REQUIRE(detail::pack_samples(rgba, 4, 2, 1, 4, img8));
  REQUIRE(detail::encode_png(img8, png, err));
  CHECK(chunks(png).at(0).data.at(9) == 6);
  CHECK(decode(png, LCT_RGBA, 8).samples == rgba);

  detail::PngImage bad;
  CHECK_FALSE(detail::pack_samples(rgba, 4, 3, 1, 3, bad));  // size mismatch
  CHECK_FALSE(detail::encode_png(bad, png, err));
}

TEST_CASE("render_test_export: default file name", "[fast][render][export]") {
  CHECK(detail::default_export_name("clip", 12, 10, ExportKind::HdrPng16) == "clip_12_10_HdrPng16.png");
  CHECK(detail::default_export_name("s", 0, 0, ExportKind::ViewPng8) == "s_0_0_ViewPng8.png");
  CHECK(detail::default_export_name("s", 3, 4, ExportKind::ScopeCiePng) == "s_3_4_ScopeCiePng.png");
}

// ---- GPU half -------------------------------------------------------------------------------------------------------

// AT1: Given pq_patches_1000, When HdrPng16, Then the PNG has cICP = 09 10 00 01 and the 1000-nit patch value is within
// ±32/65535 of 0.7518. Also: the export is byte-identical when repeated (§10.9.2 item 5), native resolution, no cICP
// after IDAT, and no mDCV/cLLi for a source without static metadata.
TEST_CASE("render_test_export: HdrPng16 of pq_patches_1000 has cICP 09 10 00 01 and PQ 0.7518 at 1000 nits (AT1)",
          "[fast][render][vulkan][export]") {
  const nlohmann::json exp = expected("pq_patches_1000");
  const OwnedPicture op = pq_patches(exp);
  auto surface = surface_or_skip(640, 360);
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  const std::string path = temp_png("hdr16_pq");
  REQUIRE(surface->export_png(path.c_str(), ExportKind::HdrPng16, nullptr));
  CHECK(std::string(surface->last_error()).empty());
  const std::vector<uint8_t> png = read_bytes(path);

  const std::vector<Chunk> cs = chunks(png);
  REQUIRE(find_chunk(cs, "IHDR") == 0);
  CHECK(be32(cs[0].data, 0) == exp.at("width").get<uint32_t>());  // native resolution, not the 640x360 viewport
  CHECK(be32(cs[0].data, 4) == exp.at("height").get<uint32_t>());
  CHECK(cs[0].data.at(8) == 16);
  CHECK(cs[0].data.at(9) == 2);
  const int cicp = find_chunk(cs, "cICP");
  REQUIRE(cicp > 0);
  CHECK(cicp < find_chunk(cs, "IDAT"));
  CHECK(cs[static_cast<size_t>(cicp)].data == std::vector<uint8_t>{0x09, 0x10, 0x00, 0x01});
  CHECK(find_chunk(cs, "mDCV") < 0);
  CHECK(find_chunk(cs, "cLLI") < 0);

  const Decoded d = decode(png, LCT_RGB, 16);
  const auto& col = exp.at("patch_columns")[3];  // the 1000-nit patch
  const auto& nits = exp.at("nits");
  REQUIRE(nits[3].get<double>() == 1000.0);
  const double want = 0.7518 * 65535.0;
  REQUIRE(std::abs(pq_oetf(1000.0) - 0.7518) < 5e-4);
  const unsigned x = (col[0].get<unsigned>() + col[1].get<unsigned>()) / 2;
  for (const unsigned y : {0U, d.height / 2, d.height - 1}) {
    for (unsigned c = 0; c < 3; ++c) {
      INFO("pixel (" << x << ", " << y << ") channel " << c << " = " << rgb16(d, x, y, c));
      CHECK(std::abs(static_cast<double>(rgb16(d, x, y, c)) - want) <= 32.0);
    }
  }

  // Deterministic: the same picture exports to the same bytes.
  const std::string again = temp_png("hdr16_pq_again");
  REQUIRE(surface->export_png(again.c_str(), ExportKind::HdrPng16, nullptr));
  CHECK(read_bytes(again) == png);
  std::filesystem::remove(path);
  std::filesystem::remove(again);
}

// The source's static HDR metadata goes into mDCV and cLLi; crop applied (§10.8).
TEST_CASE("render_test_export: HdrPng16 writes mDCV and cLLi and applies the crop", "[fast][render][vulkan][export]") {
  OwnedPicture op = bt2020_picture(64, 48, 16, 723);
  op.pic.color.mdcv_primaries[0][0] = 35400;  // 0.00002 units, always R, G, B (chapter 7 §7.3.5 row 15)
  op.pic.color.mdcv_primaries[0][1] = 14600;
  op.pic.color.mdcv_primaries[1][0] = 8500;
  op.pic.color.mdcv_primaries[1][1] = 39850;
  op.pic.color.mdcv_primaries[2][0] = 6550;
  op.pic.color.mdcv_primaries[2][1] = 2300;
  op.pic.color.mdcv_white_point[0] = 15635;
  op.pic.color.mdcv_white_point[1] = 16450;
  op.pic.color.mdcv_max_luminance = 10000000;  // 1000 cd/m² in 0.0001 units
  op.pic.color.mdcv_min_luminance = 50;
  op.pic.color.max_cll = 1000;
  op.pic.color.max_fall = 400;
  op.pic.crop = ovt::fb::Rect(8, 4, 40, 30);
  auto surface = surface_or_skip(64, 48);
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  const std::string path = temp_png("hdr16_meta");
  REQUIRE(surface->export_png(path.c_str(), ExportKind::HdrPng16, nullptr));
  const std::vector<uint8_t> png = read_bytes(path);
  std::filesystem::remove(path);
  const std::vector<Chunk> cs = chunks(png);
  CHECK(be32(cs.at(0).data, 0) == 40);
  CHECK(be32(cs.at(0).data, 4) == 30);
  const int mdcv = find_chunk(cs, "mDCV");
  const int clli = find_chunk(cs, "cLLI");
  REQUIRE(mdcv > 0);
  REQUIRE(clli > 0);
  const std::vector<uint8_t>& m = cs[static_cast<size_t>(mdcv)].data;
  REQUIRE(m.size() == 24);
  auto be16 = [&m](size_t at) { return (unsigned{m.at(at)} << 8U) | m.at(at + 1); };
  CHECK(be16(0) == 35400);   // red x
  CHECK(be16(4) == 8500);    // green x
  CHECK(be16(8) == 6550);    // blue x
  CHECK(be16(12) == 15635);  // white x
  CHECK(be32(m, 16) == 10000000);
  CHECK(be32(m, 20) == 50);
  CHECK(be32(cs[static_cast<size_t>(clli)].data, 0) == 10000000);
  CHECK(be32(cs[static_cast<size_t>(clli)].data, 4) == 4000000);

  // The mastering metadata does not tone-map the pass-through: the 1000-nit code still lands on PQ 0.7518.
  const Decoded d = decode(png, LCT_RGB, 16);
  CHECK(std::abs(static_cast<double>(rgb16(d, 20, 15, 1)) - (0.7518 * 65535.0)) <= 32.0);
}

TEST_CASE("render_test_export: HdrPng16 of HLG and SDR sources", "[fast][render][vulkan][export]") {
  auto surface = surface_or_skip(32, 32);
  const OwnedPicture hlg = bt2020_picture(32, 16, 18, 700);
  surface->set_frame(hlg.pic);
  const std::string path = temp_png("hdr16_other");
  REQUIRE(surface->export_png(path.c_str(), ExportKind::HdrPng16, nullptr));
  std::vector<Chunk> cs = chunks(read_bytes(path));
  CHECK(cs[static_cast<size_t>(find_chunk(cs, "cICP"))].data == std::vector<uint8_t>{9, 18, 0, 1});

  OwnedPicture sdr = sdr_ramp(32, 16);
  sdr.pic.generation = 1;  // a new picture for the slot (§10.2.1 skips a repeated generation)
  surface->set_frame(sdr.pic);
  REQUIRE(surface->export_png(path.c_str(), ExportKind::HdrPng16, nullptr));
  cs = chunks(read_bytes(path));
  CHECK(cs.at(0).data.at(8) == 16);
  CHECK(cs[static_cast<size_t>(find_chunk(cs, "cICP"))].data == std::vector<uint8_t>{1, 13, 0, 1});
  std::filesystem::remove(path);
}

// SdrPng8 encodes what the Srgb8 offscreen surface shows for the same picture at 1:1 (the WI-1.405 AT1 comparison):
// both are one pl_render_image with the same parameters, so the RGB codes are equal.
TEST_CASE("render_test_export: SdrPng8 equals the Srgb8 render at 1:1", "[fast][render][vulkan][export]") {
  const OwnedPicture op = sdr_ramp(96, 40);
  auto surface = surface_or_skip(96, 40);
  surface->set_frame(op.pic);
  const ViewState view = one_to_one(op);
  surface->render(view);
  detail::OutputPixels shown;
  REQUIRE(detail::download_output(*surface, shown));
  REQUIRE(shown.texel_bytes == 4);

  const std::string path = temp_png("sdr8");
  REQUIRE(surface->export_png(path.c_str(), ExportKind::SdrPng8, &view));
  const std::vector<uint8_t> png = read_bytes(path);
  std::filesystem::remove(path);
  const std::vector<Chunk> cs = chunks(png);
  CHECK(cs.at(0).data.at(8) == 8);
  CHECK(cs.at(0).data.at(9) == 2);
  CHECK(cs[static_cast<size_t>(find_chunk(cs, "cICP"))].data == std::vector<uint8_t>{1, 13, 0, 1});
  const Decoded d = decode(png, LCT_RGB, 8);
  REQUIRE(d.width == 96);
  REQUIRE(d.height == 40);
  int worst = 0;
  for (size_t i = 0; i < size_t{96} * 40; ++i) {
    for (size_t c = 0; c < 3; ++c) {
      worst = std::max(worst, std::abs(int{d.samples[(i * 3) + c]} - int{shown.bytes[(i * 4) + c]}));
    }
  }
  CHECK(worst <= 1);
}

// AT2 precondition: SdrPng8 of the PQ pattern with BT.2390 is reproducible (the mpv comparison itself is the parity
// suite of WI-1.575, whose references do not exist yet; see docs/acceptance/wi/WI-1.412.md), tone-maps (the 10000-nit
// patch is not clipped to the same code as the 4000-nit one) and keeps the patches neutral grey.
TEST_CASE(
    "render_test_export: SdrPng8 of pq_patches_1000 with BT.2390 is reproducible and tone-mapped (AT2 precondition)",
    "[fast][render][vulkan][export]") {
  const nlohmann::json exp = expected("pq_patches_1000");
  const OwnedPicture op = pq_patches(exp);
  auto surface = surface_or_skip(64, 64);
  surface->set_frame(op.pic);
  ViewState view = one_to_one(op);
  view.tone_map = ToneMap::BT2390;
  const std::string a = temp_png("sdr8_pq_a");
  const std::string b = temp_png("sdr8_pq_b");
  REQUIRE(surface->export_png(a.c_str(), ExportKind::SdrPng8, &view));
  REQUIRE(surface->export_png(b.c_str(), ExportKind::SdrPng8, &view));
  const std::vector<uint8_t> png = read_bytes(a);
  CHECK(read_bytes(b) == png);
  std::filesystem::remove(a);
  std::filesystem::remove(b);
  const Decoded d = decode(png, LCT_RGB, 8);
  const auto& cols = exp.at("patch_columns");
  std::vector<int> grey;
  for (const auto& col : cols) {
    const unsigned x = (col[0].get<unsigned>() + col[1].get<unsigned>()) / 2;
    const size_t at = ((size_t{d.height / 2} * d.width) + x) * 3;
    CHECK(std::abs(int{d.samples[at]} - int{d.samples[at + 1]}) <= 1);
    CHECK(std::abs(int{d.samples[at + 2]} - int{d.samples[at + 1]}) <= 1);
    grey.push_back(d.samples[at + 1]);
  }
  for (size_t i = 1; i < grey.size(); ++i) {
    CHECK(grey[i] >= grey[i - 1]);  // monotonic in nits
  }
  CHECK(grey[4] < grey[5]);  // 4000 vs 10000 nits: compressed, not clipped
}

// ViewPng8 is the viewport as displayed: zoomed and panned, equal to the Srgb8 output of the same render.
TEST_CASE("render_test_export: ViewPng8 equals the displayed viewport", "[fast][render][vulkan][export]") {
  const OwnedPicture op = sdr_ramp(64, 32);
  auto surface = surface_or_skip(80, 50);
  surface->set_frame(op.pic);
  ViewState view;
  view.zoom = 2.0;
  view.pan_x = 20.0;
  view.pan_y = 12.0;
  view.dither = false;
  surface->render(view);
  detail::OutputPixels shown;
  REQUIRE(detail::download_output(*surface, shown));

  const std::string path = temp_png("view8");
  REQUIRE(surface->export_png(path.c_str(), ExportKind::ViewPng8, nullptr));  // the view of the last render
  const std::vector<uint8_t> png = read_bytes(path);
  std::filesystem::remove(path);
  const std::vector<Chunk> cs = chunks(png);
  CHECK(be32(cs.at(0).data, 0) == 80);
  CHECK(be32(cs.at(0).data, 4) == 50);
  CHECK(cs.at(0).data.at(9) == 2);  // opaque background: RGB
  const Decoded d = decode(png, LCT_RGB, 8);
  int worst = 0;
  for (size_t i = 0; i < size_t{80} * 50; ++i) {
    for (size_t c = 0; c < 3; ++c) {
      worst = std::max(worst, std::abs(int{d.samples[(i * 3) + c]} - int{shown.bytes[(i * 4) + c]}));
    }
  }
  CHECK(worst <= 1);
}

// A transparent background keeps the alpha channel (the overlay-only export of §10.8 draws on it).
TEST_CASE("render_test_export: ViewPng8 over a transparent background keeps alpha", "[fast][render][vulkan][export]") {
  auto surface = surface_or_skip(16, 8);
  ViewState view;
  view.background_rgba = 0x00000000;
  const std::string path = temp_png("view8_clear");
  REQUIRE(surface->export_png(path.c_str(), ExportKind::ViewPng8, &view));
  const std::vector<uint8_t> png = read_bytes(path);
  std::filesystem::remove(path);
  CHECK(chunks(png).at(0).data.at(9) == 6);
  const Decoded d = decode(png, LCT_RGBA, 8);
  CHECK(d.samples.at(3) == 0);
}

TEST_CASE("render_test_export: errors (no picture, unwritable path)", "[fast][render][vulkan][export]") {
  auto surface = surface_or_skip(16, 8);
  const std::string path = temp_png("unused");
  CHECK_FALSE(surface->export_png(path.c_str(), ExportKind::HdrPng16, nullptr));
  CHECK(std::string(surface->last_error()).find("no picture") != std::string::npos);
  CHECK_FALSE(surface->export_png(path.c_str(), ExportKind::SdrPng8, nullptr));

  const OwnedPicture op = sdr_ramp(16, 8);
  surface->set_frame(op.pic);
  const std::string bad = (std::filesystem::temp_directory_path() / "ovt_no_such_dir" / "x.png").string();
  CHECK_FALSE(surface->export_png(bad.c_str(), ExportKind::SdrPng8, nullptr));
  CHECK(std::string(surface->last_error()).find("cannot write") != std::string::npos);
  CHECK_FALSE(surface->export_png(nullptr, ExportKind::ViewPng8, nullptr));
}
