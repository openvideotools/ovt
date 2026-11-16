// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.434 (chapter 10 §10.11): MV arrow pass by vertex pulling and intra direction strokes (§10.5.1, §10.5.4,
// §10.5.6 passes 8-9, §10.5.8 R-MV-1 / R-8K-1), with the golden tests/golden/render/synthetic_4k_mv.png.gz.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fixtures/make_synthetic.h"
#include "overlay/arrow_instances.h"
#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"
#include "ovt_model_generated.h"
#include "png_io.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

namespace fb = ovt::fb;
using ovt::render::OverlayBuilder;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::ArrowInstances;
using ovt::render::detail::IntraCodec;
using ovt::render::detail::OutputPixels;
using ovt::render::test::OwnedPicture;
using ovt::render::test::RgbImage;
using ovt::render::test::surface_or_skip;

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

constexpr uint32_t kAllMv = bit(OverlayLayer::MvL0) | bit(OverlayLayer::MvL1);

struct Mv {
  uint8_t list = 0;
  int x = 0, y = 0, w = 8, h = 8;  // NOLINT(readability-isolate-declaration)
  int16_t mv_x = 0;
  int16_t mv_y = 0;
  uint16_t flags = 0;
};

// A FrameAnalysis of cells_w x cells_h cells with the given MotionVectors and, optionally, intra modes and blocks.
struct Fa {
  uint32_t cells_w = 16;
  uint32_t cells_h = 16;
  std::string plugin = "ovt.plugin.de265";
  std::vector<Mv> mvs;
  std::vector<int16_t> intra;      // cells entries or empty
  std::vector<uint32_t> block_id;  // cells entries or empty
  std::vector<fb::Rect> blocks;    // Block i has rect blocks[i]

  [[nodiscard]] std::vector<uint8_t> bytes() const {
    fb::FrameAnalysisT fa;
    fa.schema_version = 1;
    fa.plugin_id = plugin;
    fa.grid = std::make_unique<fb::CellGridT>();
    fa.grid->cells_w = cells_w;
    fa.grid->cells_h = cells_h;
    fa.grid->intra_mode_y = intra;
    fa.grid->block_id = block_id;
    for (size_t i = 0; i < blocks.size(); ++i) {
      auto b = std::make_unique<fb::BlockT>();
      b->id = static_cast<uint32_t>(i);
      b->rect = std::make_unique<fb::Rect>(blocks[i]);
      fa.blocks.push_back(std::move(b));
    }
    for (const Mv& m : mvs) {
      auto v = std::make_unique<fb::MotionVectorT>();
      v->list = m.list;
      v->rect = std::make_unique<fb::Rect>(m.x, m.y, m.w, m.h);
      v->mv_x = m.mv_x;
      v->mv_y = m.mv_y;
      v->flags = m.flags;
      fa.mvs.push_back(std::move(v));
    }
    flatbuffers::FlatBufferBuilder fbb;
    fb::FinishFrameAnalysisBuffer(fbb, fb::FrameAnalysis::Pack(fbb, &fa));
    const std::span<const uint8_t> b(fbb.GetBufferPointer(), fbb.GetSize());
    return {b.begin(), b.end()};
  }
};

ArrowInstances instances_of(const Fa& f, const OverlayConfig& cfg, std::vector<std::string>* diag = nullptr) {
  const std::vector<uint8_t> buf = f.bytes();
  ArrowInstances out;
  std::vector<std::string> d;
  ovt::render::detail::build_arrow_instances(*fb::GetFrameAnalysis(buf.data()), cfg, out, d);
  if (diag != nullptr) {
    *diag = d;
  }
  return out;
}

uint32_t color_of(const ArrowInstances& a, size_t i) {
  return a.attr.at(i * 4) & 0xFFU;
}
uint32_t flags_of(const ArrowInstances& a, size_t i) {
  return a.attr.at(i * 4) >> 8U;
}
uint32_t id_of(const ArrowInstances& a, size_t i) {
  return a.attr.at((i * 4) + 1);
}

// --- GPU helpers -----------------------------------------------------------------------------------------------

OwnedPicture grey_picture(uint32_t w, uint32_t h) {
  OwnedPicture op(w, h, 8, 1, 1);
  op.fill_plane(0, 126);
  op.fill_plane(1, 128);
  op.fill_plane(2, 128);
  op.pic.generation = 1;
  return op;
}

ViewState centred(double zoom, double cx, double cy) {
  ViewState view;
  view.zoom = zoom;
  view.pan_x = cx;
  view.pan_y = cy;
  view.dither = false;
  return view;
}

OutputPixels render_fa(VideoSurface& surface, const std::vector<uint8_t>& fa, const OverlayConfig& cfg,
                       const ViewState& view) {
  OverlayBuilder builder(surface);
  auto set = builder.build(fa.data(), fa.size(), cfg);
  REQUIRE(set != nullptr);
  surface.set_overlays(std::move(set));
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

bool near(const std::array<int, 3>& a, uint32_t rgba) {
  const std::array<int, 3> e = {static_cast<int>(rgba >> 24U), static_cast<int>((rgba >> 16U) & 0xFFU),
                                static_cast<int>((rgba >> 8U) & 0xFFU)};
  for (size_t c = 0; c < 3; ++c) {
    if (std::abs(a.at(c) - e.at(c)) > 2) {
      return false;
    }
  }
  return true;
}

constexpr uint32_t kOrange = 0xFF851BFFU;  // §10.5.2 row 1
constexpr uint32_t kPurple = 0xB10DC9FFU;  // row 2
constexpr uint32_t kCyan = 0x00E5FFFFU;    // row 3
constexpr uint32_t kGreen = 0x2ECC40FFU;   // row 20

int count_near(const OutputPixels& px, uint32_t rgba) {
  int n = 0;
  for (int y = 0; y < px.height; ++y) {
    for (int x = 0; x < px.width; ++x) {
      n += near(rgb_at(px, x, y), rgba) ? 1 : 0;
    }
  }
  return n;
}

RgbImage to_rgb(const OutputPixels& px, int x0 = 0, int y0 = 0, int w = -1, int h = -1) {
  w = w < 0 ? px.width : w;
  h = h < 0 ? px.height : h;
  RgbImage img;
  img.width = w;
  img.height = h;
  img.rgb.reserve(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  for (int y = y0; y < y0 + h; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      const auto c = rgb_at(px, x, y);
      img.rgb.insert(img.rgb.end(),
                     {static_cast<uint8_t>(c[0]), static_cast<uint8_t>(c[1]), static_cast<uint8_t>(c[2])});
    }
  }
  return img;
}

// A golden over 2 MB is stored gzip-compressed (`<name>.png.gz`, .pre-commit-config.yaml / chapter 14 §14.4): the
// stored-deflate PNG of a 1920x1080 overlay is 6 MB. gzip is on every desktop CI image (as for the metrics goldens);
// -n keeps the bytes reproducible. The plain PNG goes to the working directory.
bool run_gzip(const std::string& args) {
  const std::string cmd = "gzip " + args;
  return std::system(cmd.c_str()) == 0;  // NOLINT(cert-env33-c,concurrency-mt-unsafe): test-only, fixed arguments
}

// §10.9.2 item 4 comparison rule (same as the WI-2.431 tests): a pixel differs when a channel is > 2 codes off; at
// most 0.1 % may differ. OVT_UPDATE_GOLDEN=1 rewrites the golden (never on CI).
void check_golden(const RgbImage& actual, const std::string& name) {
  const std::string golden = std::string(OVT_RENDER_GOLDEN_DIR) + "/" + name + ".gz";
  const std::string& plain = name;                        // decompressed copy in the working directory
  const char* update = std::getenv("OVT_UPDATE_GOLDEN");  // NOLINT(concurrency-mt-unsafe): test setup
  const char* ci = std::getenv("CI");                     // NOLINT(concurrency-mt-unsafe)
  if (update != nullptr && std::string(update) == "1" && (ci == nullptr || std::string(ci).empty())) {
    REQUIRE(ovt::render::test::write_png(plain, actual));
    REQUIRE(run_gzip("-n -9 -c '" + plain + "' > '" + golden + "'"));
    WARN("rewrote golden " << golden);
    return;
  }
  REQUIRE(run_gzip("-dc '" + golden + "' > '" + plain + "'"));
  const std::optional<RgbImage> read = ovt::render::test::read_png(plain);
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

// synthetic_4k_mv.fb: the generated fixture when present (tests/corpora/synthetic/generate.py --fixtures writes it),
// else its in-memory twin (make_synthetic_mv), which draws the same arrows.
std::vector<uint8_t> synthetic_4k_mv() {
  std::ifstream in(std::string(OVT_RENDER_FIXTURE_DIR) + "/synthetic_4k_mv.fb", std::ios::binary);
  if (in) {
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    flatbuffers::Verifier::Options opts;
    opts.max_tables = size_t{1} << 28U;
    flatbuffers::Verifier v(bytes.data(), bytes.size(), opts);
    if (fb::VerifyFrameAnalysisBuffer(v)) {
      return bytes;
    }
  }
  return ovt::render::test::make_synthetic_mv(3840, 2160);
}

}  // namespace

// ---- CPU: instance builder (steps 1, 4, 5) ------------------------------------------------------------------

TEST_CASE("render_test_arrows: instance segments, colours and flags (§10.5.4)", "[fast][render][overlay]") {
  Fa f;
  f.mvs = {
      {0, 0, 0, 8, 8, 16, -32, 0},        // 0: L0 uni
      {1, 8, 0, 8, 8, 32, 0, 0},          // 1: L1 uni
      {0, 16, 0, 8, 8, -16, 0, 0x1},      // 2: L0 of a bi block
      {1, 16, 0, 8, 8, 16, 16, 0x1},      // 3: L1 of a bi block
      {0, 24, 0, 8, 8, 0, 0, 0x4 | 0x2},  // 4: L0 global/warped, weighted
      {2, 0, 8, 8, 8, 0, 0, 0},           // 5: list 2: not drawn
      {1, 32, 0, 4, 4, 0, 8, 0x10},       // 6: L1 field
  };
  OverlayConfig cfg;
  cfg.layers = kAllMv;
  cfg.mv_scale = 2.0F;
  std::vector<std::string> diag;
  const ArrowInstances a = instances_of(f, cfg, &diag);
  REQUIRE(a.size() == 6);
  // Segments: L1 uni {1, 6}, L1 bi {3}, L0 bi {2}, L0 uni {0, 4}.
  CHECK(a.seg == ovt::render::detail::ArrowSegments{2, 3, 4, 6, 6});
  const std::array<uint32_t, 6> ids = {1, 6, 3, 2, 0, 4};
  for (size_t i = 0; i < ids.size(); ++i) {
    CHECK(id_of(a, i) == ids.at(i));
  }
  CHECK(color_of(a, 0) == 2);
  CHECK(flags_of(a, 0) == ovt::render::detail::kInstDashed);
  CHECK(flags_of(a, 1) == (ovt::render::detail::kInstDashed | ovt::render::detail::kInstField));
  CHECK(flags_of(a, 2) == (ovt::render::detail::kInstDashed | ovt::render::detail::kInstBi));
  CHECK(color_of(a, 3) == 1);
  CHECK(flags_of(a, 3) == ovt::render::detail::kInstBi);
  CHECK(color_of(a, 5) == 21);  // global wins; MotionVector.flags bit1 (weighted) is not the dash bit
  CHECK(flags_of(a, 5) == ovt::render::detail::kInstGlobal);
  // Origin = rect centre; vector = mv / 16 * mv_scale.
  CHECK(a.geom.at(16) == 4.0F);  // instance 4 = mvs[0]
  CHECK(a.geom.at(17) == 4.0F);
  CHECK(a.geom.at(18) == 2.0F);   // 16 / 16 * 2
  CHECK(a.geom.at(19) == -4.0F);  // -32 / 16 * 2
  REQUIRE(diag.size() == 2);
  CHECK(diag[0].find("1 MotionVector") != std::string::npos);
  CHECK(diag[1].rfind("RND-0060 ", 0) == 0);  // mvs[6] is a field MV: one note per build (§10.10)

  // No MV layer: no arrows. Determinism and thread independence: a large set builds the same in any chunking, so a
  // repeated build is byte-identical.
  cfg.layers = bit(OverlayLayer::Heatmap);
  CHECK(instances_of(f, cfg).size() == 0);
}

TEST_CASE("render_test_arrows: threaded build keeps the MotionVector order", "[fast][render][overlay]") {
  const std::vector<uint8_t> buf = ovt::render::test::make_synthetic_mv(1920, 1080);
  const auto* fa = fb::GetFrameAnalysis(buf.data());
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::MvL0);
  ArrowInstances a;
  ArrowInstances b;
  std::vector<std::string> diag;
  ovt::render::detail::build_arrow_instances(*fa, cfg, a, diag);
  ovt::render::detail::build_arrow_instances(*fa, cfg, b, diag);
  REQUIRE(a.size() == size_t{480} * 270);
  CHECK(a.seg.l0_uni == 0);
  CHECK(a.geom == b.geom);
  CHECK(a.attr == b.attr);
  for (size_t i = 0; i < a.size(); ++i) {
    if (id_of(a, i) != i) {
      FAIL("instance " << i << " has id " << id_of(a, i));
    }
  }
  CHECK(diag.empty());
}

// Above parallel_ranges' 4096-item threshold with every class mixed: the threaded scatter must equal a sequential
// stable partition by segment (review note on #647).
TEST_CASE("render_test_arrows: threaded build of mixed lists equals a stable partition", "[fast][render][overlay]") {
  Fa f;
  for (int i = 0; i < 6007; ++i) {
    const auto list = static_cast<uint8_t>(i % 3 == 0 ? 1 : 0);
    const auto flags = static_cast<uint16_t>(i % 5 == 0 ? 0x1 : 0);
    f.mvs.push_back({list, (i % 64) * 8, (i / 64) * 8, 8, 8, static_cast<int16_t>(i % 97), 0, flags});
  }
  OverlayConfig cfg;
  cfg.layers = kAllMv;
  const ArrowInstances a = instances_of(f, cfg);
  REQUIRE(a.size() == f.mvs.size());
  // Reference: segment of each MV (L1 uni, L1 bi, L0 bi, L0 uni), then MV order inside a segment.
  std::vector<uint32_t> ref(f.mvs.size());
  for (uint32_t i = 0; i < ref.size(); ++i) {
    ref[i] = i;
  }
  auto seg = [&](uint32_t i) {
    const bool bi = (f.mvs[i].flags & 1U) != 0;
    return f.mvs[i].list == 1 ? (bi ? 1 : 0) : (bi ? 2 : 3);
  };
  std::stable_sort(ref.begin(), ref.end(), [&](uint32_t x, uint32_t y) { return seg(x) < seg(y); });
  for (size_t i = 0; i < ref.size(); ++i) {
    if (id_of(a, i) != ref[i]) {
      FAIL("instance " << i << " is mvs[" << id_of(a, i) << "], expected mvs[" << ref[i] << "]");
    }
  }
  CHECK(a.seg.l1_bi ==
        static_cast<uint32_t>(std::count_if(ref.begin(), ref.end(), [&](uint32_t i) { return seg(i) == 0; })));
}

TEST_CASE("render_test_arrows: intra directions per codec (§6.3.1 numbering)", "[fast][render][overlay]") {
  using ovt::render::detail::intra_direction;
  auto dir = [](IntraCodec c, int m, int delta = 0) {
    const auto d = intra_direction(c, m, delta);
    REQUIRE(d.has_value());
    return d.value_or(std::array<float, 2>{});
  };
  const float r = std::sqrt(0.5F);
  // HEVC: 26 vertical, 10 horizontal, 2 bottom-left, 18 top-left, 34 top-right; planar/DC/PCM none.
  CHECK(dir(IntraCodec::H265, 26)[0] == Catch::Approx(0.0F).margin(1e-6));
  CHECK(dir(IntraCodec::H265, 26)[1] == Catch::Approx(-1.0F));
  CHECK(dir(IntraCodec::H265, 10)[0] == Catch::Approx(-1.0F));
  CHECK(dir(IntraCodec::H265, 2)[0] == Catch::Approx(-r));
  CHECK(dir(IntraCodec::H265, 2)[1] == Catch::Approx(r));
  CHECK(dir(IntraCodec::H265, 18)[1] == Catch::Approx(-r));
  CHECK(dir(IntraCodec::H265, 34)[0] == Catch::Approx(r));
  for (const int m : {0, 1, 35, 64, 65, -1}) {
    CHECK_FALSE(intra_direction(IntraCodec::H265, m).has_value());
  }
  // H.264: 0 V, 1 H, 3 DDL, 16 + 0 V; DC, plane, PCM none.
  CHECK(dir(IntraCodec::H264, 0)[1] == Catch::Approx(-1.0F));
  CHECK(dir(IntraCodec::H264, 1)[0] == Catch::Approx(-1.0F));
  CHECK(dir(IntraCodec::H264, 3)[0] == Catch::Approx(r));
  CHECK(dir(IntraCodec::H264, 16)[1] == Catch::Approx(-1.0F));
  for (const int m : {2, 18, 19, 32}) {
    CHECK_FALSE(intra_direction(IntraCodec::H264, m).has_value());
  }
  // AV1: V_PRED 90°, H_PRED 180°, D45; angle_delta 3 on V_PRED = 99°.
  CHECK(dir(IntraCodec::AV1, 1)[1] == Catch::Approx(-1.0F));
  CHECK(dir(IntraCodec::AV1, 2)[0] == Catch::Approx(-1.0F));
  CHECK(dir(IntraCodec::AV1, 3)[0] == Catch::Approx(r));
  CHECK(dir(IntraCodec::AV1, 1, 3)[0] == Catch::Approx(std::cos(99.0 * 3.14159265358979 / 180.0)));
  for (const int m : {0, 9, 12, 32, 33}) {
    CHECK_FALSE(intra_direction(IntraCodec::AV1, m).has_value());
  }
  CHECK_FALSE(intra_direction(IntraCodec::Unknown, 26).has_value());
}

TEST_CASE("render_test_arrows: one stroke per uniform block, else per cell", "[fast][render][overlay]") {
  Fa f;
  f.cells_w = 4;
  f.cells_h = 2;
  // Block 0 = left 8x8 (cells 0,1 / 4,5), uniform mode 26; block 1 = right 8x8, mixed 10 / 1 (DC) / 2 / -1.
  f.block_id = {0, 0, 1, 1, 0, 0, 1, 1};
  f.blocks = {fb::Rect(0, 0, 8, 8), fb::Rect(8, 0, 8, 8)};
  f.intra = {26, 26, 10, 1, 26, 26, 2, -1};
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::IntraDirs);
  const ArrowInstances a = instances_of(f, cfg);
  REQUIRE(a.size() == 3);
  CHECK(a.seg.intra == 0);
  CHECK(a.seg.end == 3);
  // Block 0: vertical stroke through (4, 4), 80 % of 8 samples long.
  CHECK(id_of(a, 0) == 0);
  CHECK(color_of(a, 0) == 20);
  CHECK(flags_of(a, 0) == ovt::render::detail::kInstIntraStroke);
  CHECK(a.geom.at(0) == Catch::Approx(4.0F));
  CHECK(a.geom.at(1) == Catch::Approx(7.2F));
  CHECK(a.geom.at(2) == Catch::Approx(0.0F).margin(1e-6));
  CHECK(a.geom.at(3) == Catch::Approx(-6.4F));
  // Mixed block 1: per cell, raster order: cell 2 (mode 10), cell 6 (mode 2); DC and -1 have none.
  CHECK(id_of(a, 1) == (2U | 0x80000000U));
  CHECK(a.geom.at(4) == Catch::Approx(8.0F + 2.0F + 1.6F));  // horizontal, centred on (10, 2)
  CHECK(id_of(a, 2) == (6U | 0x80000000U));
  // Unknown codec: no strokes, one note.
  f.plugin = "something";
  std::vector<std::string> diag;
  CHECK(instances_of(f, cfg, &diag).size() == 0);
  CHECK(diag.size() == 1);
}

TEST_CASE("render_test_arrows: R-8K-1 viewport-culled subset keeps the segments", "[fast][render][overlay]") {
  Fa f;
  f.mvs = {{1, 0, 0, 8, 8, 0, 0, 0},
           {1, 1000, 0, 8, 8, 0, 0, 0},
           {0, 1000, 1000, 8, 8, 0, 0, 0x1},
           {0, 0, 0, 8, 8, -320, 0, 0},
           {0, 50, 0, 8, 8, 0, 0, 0}};
  OverlayConfig cfg;
  cfg.layers = kAllMv;
  const ArrowInstances all = instances_of(f, cfg);
  ArrowInstances cut;
  ovt::render::detail::cull_instances(all, {0.0F, 0.0F, 100.0F, 100.0F}, cut);
  // Kept: mvs[0] (L1 uni), mvs[3] (origin (4, 4) pointing left out of the rect: still intersects), mvs[4].
  REQUIRE(cut.size() == 3);
  CHECK(cut.seg == ovt::render::detail::ArrowSegments{1, 1, 1, 3, 3});
  CHECK(id_of(cut, 0) == 0);
  CHECK(id_of(cut, 1) == 3);
  CHECK(id_of(cut, 2) == 4);
  // An arrow whose shaft crosses the rect from outside is kept too.
  ovt::render::detail::cull_instances(all, {-30.0F, 0.0F, -10.0F, 10.0F}, cut);
  REQUIRE(cut.size() == 1);
  CHECK(id_of(cut, 0) == 3);
}

// ---- GPU: the pass on lavapipe / MoltenVK ------------------------------------------------------------------------

// Acceptance test 2: MV (16, 0) = 1 luma sample at zoom 1 is a 2x2 px dot (mv_min_px = 2).
TEST_CASE("render_test_arrows: a 1-sample MV is a 2x2 dot (AT2)", "[fast][render][vulkan][overlay]") {
  const OwnedPicture pic = grey_picture(64, 64);
  auto surface = surface_or_skip(64, 64);
  surface->set_frame(pic.pic);
  Fa f;
  f.mvs = {{0, 24, 24, 8, 8, 16, 0, 0}};  // centre (28, 28)
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::MvL0);
  REQUIRE(cfg.mv_min_px == 2.0F);
  const OutputPixels px = render_fa(*surface, f.bytes(), cfg, centred(1.0, 32.0, 32.0));
  CHECK(count_near(px, kOrange) == 4);
  for (int y = 27; y <= 28; ++y) {
    for (int x = 27; x <= 28; ++x) {
      CHECK(near(rgb_at(px, x, y), kOrange));
    }
  }
  // Two samples long (32, 0) is an arrow, not a dot: shaft + head.
  f.mvs = {{0, 24, 24, 8, 8, 32 * 8, 0, 0}};  // 16 samples to the right
  const OutputPixels arrow = render_fa(*surface, f.bytes(), cfg, centred(1.0, 32.0, 32.0));
  CHECK(near(rgb_at(arrow, 30, 28), kOrange));  // shaft on the row of the origin
  CHECK(near(rgb_at(arrow, 42, 28), kOrange));  // head near the tip (28 + 16 = 44)
  CHECK_FALSE(near(rgb_at(arrow, 30, 25), kOrange));
  CHECK(count_near(arrow, kOrange) > 20);
}

TEST_CASE("render_test_arrows: list coding, MvBi, strokes and R-MV-1 (§10.5.4)", "[fast][render][vulkan][overlay]") {
  const OwnedPicture pic = grey_picture(128, 64);
  auto surface = surface_or_skip(128, 64);
  surface->set_frame(pic.pic);
  Fa f;
  f.cells_w = 32;
  f.cells_h = 16;
  // A horizontal L1 arrow of 40 samples on row 12 and an L0 arrow of a bi block on row 36.
  f.mvs = {{1, 0, 8, 8, 8, 40 * 16, 0, 0}, {0, 0, 32, 8, 8, 40 * 16, 0, 0x1}};
  const ViewState view = centred(1.0, 64.0, 32.0);

  SECTION("L1 shafts are dashed 4 on / 2 off, L0 solid") {
    OverlayConfig cfg;
    cfg.layers = kAllMv;
    const OutputPixels px = render_fa(*surface, f.bytes(), cfg, view);
    int on = 0;
    int off = 0;
    for (int x = 5; x < 36; ++x) {
      (near(rgb_at(px, x, 12), kPurple) ? on : off)++;
    }
    CHECK(on > 15);
    CHECK(off > 6);  // gaps
    for (int x = 5; x < 36; ++x) {
      CHECK(near(rgb_at(px, x, 36), kOrange));  // solid
    }
  }
  SECTION("MvBi draws the bi block's arrow in cyan, solid") {
    OverlayConfig cfg;
    cfg.layers = kAllMv | bit(OverlayLayer::MvBi);
    const OutputPixels px = render_fa(*surface, f.bytes(), cfg, view);
    CHECK(near(rgb_at(px, 20, 36), kCyan));
    CHECK(count_near(px, kOrange) == 0);
    CHECK(count_near(px, kPurple) > 15);  // the uni L1 arrow keeps its colour
  }
  SECTION("Only MvL0: no L1 arrow") {
    OverlayConfig cfg;
    cfg.layers = bit(OverlayLayer::MvL0);
    const OutputPixels px = render_fa(*surface, f.bytes(), cfg, view);
    CHECK(count_near(px, kPurple) == 0);
    CHECK(count_near(px, kOrange) > 30);
  }
  SECTION("R-MV-1: no arrows when a cell is under 4 device px") {
    OverlayConfig cfg;
    cfg.layers = kAllMv;
    const OutputPixels px = render_fa(*surface, f.bytes(), cfg, centred(0.5, 64.0, 32.0));
    CHECK(count_near(px, kOrange) == 0);
    CHECK(count_near(px, kPurple) == 0);
  }
  SECTION("intra strokes in colour 20") {
    Fa g;
    g.cells_w = 32;
    g.cells_h = 16;
    g.intra.assign(size_t{32} * 16, -1);
    g.block_id.assign(size_t{32} * 16, 0);
    g.blocks = {fb::Rect(0, 0, 128, 64)};
    for (uint32_t cy = 0; cy < 16; ++cy) {
      for (uint32_t cx = 0; cx < 32; ++cx) {
        g.intra[(cy * 32) + cx] = 26;  // one vertical stroke through the whole 128 x 64 block
      }
    }
    OverlayConfig cfg;
    cfg.layers = bit(OverlayLayer::IntraDirs);
    const OutputPixels px = render_fa(*surface, g.bytes(), cfg, view);
    CHECK(near(rgb_at(px, 64, 32), kGreen));
    CHECK(near(rgb_at(px, 64, 10), kGreen));
    CHECK_FALSE(near(rgb_at(px, 64, 2), kGreen));  // 80 % of 64: from y = 6.4 to 57.6
    CHECK_FALSE(near(rgb_at(px, 70, 32), kGreen));
  }
}

// SPEC-ASSUMPTION(#645): the golden is §10.9.2's 1080p viewport; the 4K AT1 test compares its centre with it.
// The overlay golden (§10.9.2 item 4: 1920x1080 viewport, dpr 1): the centre of synthetic_4k_mv at zoom 1, so most of
// the frame's 518,400 instances are culled in the vertex shader and the rest are drawn. Determinism: two renders
// are byte-identical (§10.9.2 item 5).
TEST_CASE("render_test_arrows: synthetic_4k_mv golden (§10.9.2)", "[fast][render][vulkan][overlay]") {
  const std::vector<uint8_t> fa = synthetic_4k_mv();
  const OwnedPicture pic = grey_picture(3840, 2160);
  auto surface = surface_or_skip(1920, 1080);
  surface->set_frame(pic.pic);
  OverlayConfig cfg;
  cfg.layers = kAllMv;
  const ViewState view = centred(1.0, 1920.0, 1080.0);
  const OutputPixels first = render_fa(*surface, fa, cfg, view);
  const OutputPixels second = render_fa(*surface, fa, cfg, view);
  CHECK(first.bytes == second.bytes);
  check_golden(to_rgb(first), "synthetic_4k_mv.png");
}

// Acceptance test 1: all 518,400 arrows at zoom 1 in a 3840x2160 viewport, on the GPU runners only (chapter 14
// §14.9.2). The central 1920x1080 of the render must match the same golden (the view is the 1080p golden's, moved
// by whole pixels), and the arrow passes' time is reported as the bench_overlay_4k_mv_arrows counter.
// SPEC-ASSUMPTION(#645): libovt-render disables libplacebo's timestamp queries (vk_no_timestamps.cpp, a
// MoltenVK use-after-free), so the counter is the median wall time of render() + readback with arrows minus without.
TEST_CASE("render_test_arrows: 518,400 arrows at 4K (AT1)", "[.gpu][render][overlay]") {
  const std::vector<uint8_t> fa = synthetic_4k_mv();
  const OwnedPicture pic = grey_picture(3840, 2160);
  auto surface = surface_or_skip(3840, 2160);
  surface->set_frame(pic.pic);
  OverlayConfig cfg;
  cfg.layers = kAllMv;
  const ViewState view = centred(1.0, 1920.0, 1080.0);
  const OutputPixels first = render_fa(*surface, fa, cfg, view);
  const OutputPixels second = render_fa(*surface, fa, cfg, view);
  CHECK(first.bytes == second.bytes);
  check_golden(to_rgb(first, 960, 540, 1920, 1080), "synthetic_4k_mv.png");

  auto median_ms = [&](uint32_t layers) {
    OverlayConfig c = cfg;
    c.layers = layers;
    OverlayBuilder builder(*surface);
    surface->set_overlays(builder.build(fa.data(), fa.size(), c));
    std::vector<double> ms;
    for (int i = 0; i < 21; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      surface->render(view);
      OutputPixels px;
      REQUIRE(ovt::render::detail::download_output(*surface, px));
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
  };
  const double with = median_ms(kAllMv);
  const double without = median_ms(0);
  WARN("bench_overlay_4k_mv_arrows arrow_pass_ms=" << (with - without) << " (render+readback " << with
                                                   << " ms with arrows, " << without << " ms without)");
}
