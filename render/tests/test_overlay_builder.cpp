// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.430 (chapter 10 §10.11): OverlayBuilder CPU side. Pure CPU, no GPU needed; the builder only keeps the
// VideoSurface that created it, so the tests use an uninitialised surface.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "fixtures/make_synthetic.h"
#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"

namespace {

using ovt::render::HeatmapSource;
using ovt::render::OverlayBuilder;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::test::expected_qp;
using ovt::render::test::make_synthetic;
using ovt::render::test::SyntheticOptions;

constexpr uint32_t layer(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

struct Built {
  std::vector<uint8_t> buffer;
  std::unique_ptr<ovt::render::OverlaySet> set;
};

Built build(const SyntheticOptions& opts, const OverlayConfig& cfg, const uint16_t* custom = nullptr) {
  Built b;
  b.buffer = make_synthetic(opts);
  auto surface = ovt::render::VideoSurface::create("overlay-test");
  REQUIRE(surface != nullptr);
  OverlayBuilder builder(*surface);
  b.set = builder.build(b.buffer.data(), b.buffer.size(), cfg, custom);
  REQUIRE(b.set != nullptr);
  return b;
}

size_t at(const ovt::render::OverlaySet& s, uint32_t cx, uint32_t cy) {
  return (static_cast<size_t>(cy) * s.cells_w) + cx;
}

}  // namespace

TEST_CASE("render_test_overlay_heatmap_qp_texel", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.layers = layer(OverlayLayer::Heatmap);
  cfg.heatmap = HeatmapSource::QP;
  const auto b = build({}, cfg);
  const auto& s = *b.set;
  REQUIRE(s.heatmap.size() == 80U * 64U);
  // Acceptance 1: texel (cx, cy) = (qp - 0) / 51 * 65535.
  const uint32_t cx = 12;
  const uint32_t cy = 9;
  const int qp = expected_qp(cx, cy);
  REQUIRE(qp == 7);  // (84 + 27) % 52
  CHECK(s.heatmap[at(s, cx, cy)] == static_cast<uint16_t>(std::lround(qp / 51.0 * 65535.0)));
  CHECK(s.heat_lo == 0.0F);
  CHECK(s.heat_hi == 51.0F);
  // The maximum QP of the fixture (51) is full scale, QP 0 is zero.
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 80; ++x) {
      if (expected_qp(x, y) == 51) {
        CHECK(s.heatmap[at(s, x, y)] == 65535);
      }
      if (expected_qp(x, y) == 0) {
        CHECK(s.heatmap[at(s, x, y)] == 0);
      }
    }
  }
}

TEST_CASE("render_test_overlay_heatmap_not_applicable_survives", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.heatmap = HeatmapSource::QP;
  const auto b = build({}, cfg);
  const auto& s = *b.set;
  CHECK(expected_qp(0, 0) == -1);
  CHECK(s.heat_valid[at(s, 0, 0)] == 0);  // qp -1 = not applicable
  CHECK(s.heat_valid[at(s, 1, 0)] == 1);
}

TEST_CASE("render_test_overlay_heatmap_av1_default_range", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.heatmap = HeatmapSource::QP;
  SyntheticOptions o;
  o.av1 = true;
  const auto b = build(o, cfg);
  CHECK(b.set->heat_hi == 255.0F);
  CHECK(b.set->heatmap[at(*b.set, 12, 9)] == static_cast<uint16_t>(std::lround(7 / 255.0 * 65535.0)));
}

TEST_CASE("render_test_overlay_heatmap_explicit_range_clamps", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.heatmap = HeatmapSource::QP;
  cfg.heatmap_min = 10;
  cfg.heatmap_max = 20;
  const auto b = build({}, cfg);
  const auto& s = *b.set;
  CHECK(s.heatmap[at(s, 12, 9)] == 0);  // qp 7 < 10 clamps to 0
  for (uint32_t x = 0; x < 80; ++x) {
    if (expected_qp(x, 0) == 15) {
      CHECK(s.heatmap[at(s, x, 0)] == static_cast<uint16_t>(std::lround(0.5 * 65535.0)));
    }
    if (expected_qp(x, 0) > 20) {
      CHECK(s.heatmap[at(s, x, 0)] == 65535);
    }
  }
}

TEST_CASE("render_test_overlay_heatmap_bits_log2_auto", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.heatmap = HeatmapSource::BitsX8;
  const auto b = build({}, cfg);
  const auto& s = *b.set;
  // bits_x8 = 8 cx cy, so bits = cx cy and the log2 value is log2(1 + cx cy); auto range = min..max over the frame.
  const double lo = 0.0;  // cx cy = 0 on the first row / column
  const double hi = std::log2(1.0 + (79.0 * 63.0));
  CHECK(s.heat_lo == static_cast<float>(lo));
  CHECK(s.heat_hi == static_cast<float>(hi));
  CHECK(s.heatmap[at(s, 79, 63)] == 65535);
  CHECK(s.heatmap[at(s, 0, 40)] == 0);
  CHECK(s.heatmap[at(s, 10, 10)] == static_cast<uint16_t>(std::lround(std::log2(101.0) / hi * 65535.0)));
}

TEST_CASE("render_test_overlay_heatmap_mv_magnitude_and_txsize", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.heatmap = HeatmapSource::MvMagnitude;
  auto b = build({}, cfg);
  auto& s = *b.set;
  // ref0 < 0 (cx % 3 == 0) has no list-0 motion: not applicable.
  CHECK(s.heat_valid[at(s, 3, 3)] == 0);
  REQUIRE(s.heat_valid[at(s, 4, 3)] == 1);
  const double mag = std::sqrt(((4.0 * 4 - 100) * (4.0 * 4 - 100)) + 9.0) / 16.0;  // mv0 = (4 cx - 100, cy)
  const double lo = s.heat_lo;
  const double hi = s.heat_hi;
  CHECK(s.heatmap[at(s, 4, 3)] ==
        static_cast<uint16_t>(std::lround(std::clamp((mag - lo) / (hi - lo), 0.0, 1.0) * 65535.0)));

  cfg.heatmap = HeatmapSource::TxSize;
  b = build({}, cfg);
  CHECK(b.set->heat_lo == 4.0F);
  CHECK(b.set->heat_hi == 12.0F);
  CHECK(b.set->heat_valid[0] == 0);  // 0xFF tx size = not applicable
  CHECK(b.set->heatmap[at(*b.set, 3, 1)] ==
        static_cast<uint16_t>(std::lround((6.0 - 4.0) / 8.0 * 65535.0)));  // (2+1) + (2+1)
}

TEST_CASE("render_test_overlay_heatmap_custom_passes_through", "[render][overlay]") {
  std::vector<uint16_t> custom(80U * 64U);
  for (size_t i = 0; i < custom.size(); ++i) {
    custom[i] = static_cast<uint16_t>(i * 7U);
  }
  OverlayConfig cfg;
  cfg.heatmap = HeatmapSource::Custom;
  const auto b = build({}, cfg, custom.data());
  CHECK(b.set->heatmap == custom);
  cfg.heatmap = HeatmapSource::MetricMap;
  CHECK(build({}, cfg, custom.data()).set->heatmap == custom);
  const auto none = build({}, cfg, nullptr);  // no cells supplied: empty, with a note
  CHECK(none.set->heatmap.empty());
  CHECK_FALSE(none.set->diagnostics.empty());
}

TEST_CASE("render_test_overlay_attributes_skip_flag", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.layers = layer(OverlayLayer::SkipHatch) | layer(OverlayLayer::PredFill);
  const auto b = build({}, cfg);
  const auto& s = *b.set;
  REQUIRE(s.cell_attributes.size() == 80U * 64U * 4U);
  const auto texel = [&](uint32_t cx, uint32_t cy, int c) {
    return s.cell_attributes[(at(s, cx, cy) * 4) + static_cast<size_t>(c)];
  };
  // Acceptance 2: pred == Skip -> attribute A bit0.
  CHECK((texel(7, 5, 3) & 1U) == 1U);
  CHECK(texel(7, 5, 0) == static_cast<uint8_t>(ovt::fb::PredKind::Skip));
  // A cell whose block is flagged skip also carries bit0 (block 1 = right half), plus merge.
  CHECK((texel(60, 2, 3) & 3U) == 3U);
  // A plain inter cell in the left half: no skip, no merge.
  CHECK((texel(10, 2, 3) & 3U) == 0U);
  CHECK(texel(1, 0, 0) == static_cast<uint8_t>(ovt::fb::PredKind::Intra));
}

TEST_CASE("render_test_overlay_attributes_sentinels", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.layers = layer(OverlayLayer::RefIdxFill);
  const auto b = build({}, cfg);
  const auto& s = *b.set;
  const auto texel = [&](uint32_t cx, uint32_t cy, int c) {
    return s.cell_attributes[(at(s, cx, cy) * 4) + static_cast<size_t>(c)];
  };
  CHECK(texel(3, 0, 1) == 0);          // ref0 = -1 -> G = 0
  CHECK(texel(4, 0, 1) == 1 + 0);      // ref0 = 0 -> G = 1
  CHECK(texel(5, 0, 1) == 2);          // ref0 = 1 -> G = 2 ... cx % 3 = 2 -> ref0 1
  CHECK(texel(5, 0, 2) == 2);          // cx % 5 == 0 -> ref1 = 1 -> B = 2
  CHECK(texel(6, 0, 2) == 0);          // ref1 = -1 -> B = 0
  CHECK((texel(5, 0, 3) & 4U) == 4U);  // bi: ref0 >= 0 and ref1 >= 0
  // A plugin that provides no pred vector reports 0xFF in R.
  SyntheticOptions o;
  o.drop_pred = true;
  const auto n = build(o, cfg);
  CHECK(n.set->cell_attributes[0] == 0xFF);
}

TEST_CASE("render_test_overlay_partition_lines", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.layers = layer(OverlayLayer::GridPartition);
  const auto b = build({}, cfg);
  const auto& lines = b.set->partition_lines;
  // Two side-by-side tiles share the middle edge: 8 - 1 = 7 segments; the slice segment and tile group are not spatial.
  REQUIRE(lines.size() == 7);
  size_t middle = 0;
  for (const auto& l : lines) {
    if (l.x0 == 160 && l.x1 == 160 && l.y0 == 0 && l.y1 == 256) {
      ++middle;
    }
  }
  CHECK(middle == 1);
  // Not requested: nothing built.
  cfg.layers = 0;
  CHECK(build({}, cfg).set->partition_lines.empty());
}

TEST_CASE("render_test_overlay_malformed_input", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.heatmap = HeatmapSource::QP;
  SyntheticOptions o;
  o.short_qp = true;  // qp has cells - 1 entries: never trusted against cells_w x cells_h
  const auto b = build(o, cfg);
  CHECK(b.set->heat_valid.size() == 80U * 64U);
  CHECK_FALSE(b.set->diagnostics.empty());
  for (const uint8_t v : b.set->heat_valid) {
    CHECK(v == 0);
  }

  // Truncated and garbage buffers do not verify.
  auto surface = ovt::render::VideoSurface::create("overlay-test");
  OverlayBuilder builder(*surface);
  auto bytes = make_synthetic({});
  CHECK(builder.build(bytes.data(), bytes.size() / 2, cfg) == nullptr);
  std::vector<uint8_t> junk(64, 0xA5);
  CHECK(builder.build(junk.data(), junk.size(), cfg) == nullptr);
  CHECK(builder.build(static_cast<const uint8_t*>(nullptr), 0, cfg) == nullptr);
  // A null root still yields an (empty) set with a note rather than crashing.
  const auto empty = builder.build(static_cast<const ovt::fb::FrameAnalysis*>(nullptr), cfg);
  REQUIRE(empty != nullptr);
  CHECK_FALSE(empty->diagnostics.empty());
}

TEST_CASE("render_test_overlay_deterministic", "[render][overlay]") {
  OverlayConfig cfg;
  cfg.layers = layer(OverlayLayer::PredFill) | layer(OverlayLayer::GridPartition);
  cfg.heatmap = HeatmapSource::BitsX8;
  const auto a = build({}, cfg);
  const auto b = build({}, cfg);
  CHECK(a.set->heatmap == b.set->heatmap);
  CHECK(a.set->cell_attributes == b.set->cell_attributes);
  CHECK(a.set->heat_valid == b.set->heat_valid);
}
