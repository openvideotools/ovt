// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.434a (chapter 10 §10.11): the CPU side of the MV arrow and intra direction stroke passes (§10.5.1, §10.5.4,
// §10.5.8 R-8K-1). The GPU pass, AT1, AT2 and the golden synthetic_4k_mv.png follow in WI-2.434b.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fixtures/make_synthetic.h"
#include "overlay/arrow_instances.h"
#include "ovt/render/overlay_set.h"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::detail::ArrowInstances;
using ovt::render::detail::IntraCodec;

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
  REQUIRE(diag.size() == 1);
  CHECK(diag[0].find("1 MotionVector") != std::string::npos);

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
