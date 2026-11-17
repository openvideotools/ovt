// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.439 acceptance test 1 (chapter 10 §10.5.2, §10.5.6): the colour tables and ovt_render_legend(). CPU only.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

#include "ovt/render/overlay_colors.h"
#include "ovt/render/overlay_set.h"

namespace {

using ovt::render::kOverlayColors;
using ovt::render::kOverlayColorsCvd;
using ovt::render::LegendEntry;
using ovt::render::LegendShape;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

}  // namespace

TEST_CASE("render_test_overlay_legend: §10.5.2 table values", "[fast][render][overlay]") {
  CHECK(kOverlayColors[0] == 0x2ECC4066U);   // intra fill 40 %
  CHECK(kOverlayColors[1] == 0xFF851BFFU);   // L0 arrow
  CHECK(kOverlayColors[2] == 0xB10DC9FFU);   // L1 arrow
  CHECK(kOverlayColors[3] == 0x00E5FFFFU);   // bi arrow
  CHECK(kOverlayColors[4] == 0xAAAAAA99U);   // skip hatch
  CHECK(kOverlayColors[9] == 0xFFFF00CCU);   // grid CU
  CHECK(kOverlayColors[18] == 0x39CCCCFFU);  // selection rectangle outline
  CHECK(kOverlayColors[19] == 0xFFFFFFFFU);  // label text
  CHECK(kOverlayColors[30] == 0xE53935FFU);  // frame type I
  CHECK(kOverlayColors[31] == 0x1E88E5FFU);  // frame type P
  CHECK(kOverlayColors[32] == 0x43A047FFU);  // frame type B
  // The CVD variant changes only the coded roles (rows 0-3, 20).
  for (size_t i = 0; i < kOverlayColors.size(); ++i) {
    if (i <= 3 || i == 20) {
      CHECK(kOverlayColorsCvd.at(i) != kOverlayColors.at(i));
    } else {
      CHECK(kOverlayColorsCvd.at(i) == kOverlayColors.at(i));
    }
  }
  // Fill alphas of rows 1-3 and 18.
  CHECK(ovt::render::overlay_fill_rgba(1, false) == 0xFF851B4DU);
  CHECK(ovt::render::overlay_fill_rgba(3, true) == 0x0072B24DU);
  CHECK(ovt::render::overlay_fill_rgba(18, false) == 0x39CCCC33U);
  CHECK(ovt::render::overlay_fill_rgba(0, true) == 0x009E7366U);
  CHECK(ovt::render::overlay_fill_rgba(33, false) == 0U);
}

// AT1: layers = MvL0|MvL1|GridCU → 3 entries in draw order with the table colours.
TEST_CASE("render_test_overlay_legend: MvL0|MvL1|GridCU (WI-2.439 AT1)", "[fast][render][overlay]") {
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::MvL0) | bit(OverlayLayer::MvL1) | bit(OverlayLayer::GridCU);
  std::array<LegendEntry, 8> out{};
  REQUIRE(ovt_render_legend(cfg, false, out.data(), out.size()) == 3);
  // §10.5.6: grids (pass 5) before arrows (pass 9); arrows L1 then L0.
  CHECK(out[0].layer == OverlayLayer::GridCU);
  CHECK(out[0].color_index == 9);
  CHECK(out[0].rgba == kOverlayColors[9]);
  CHECK(out[0].shape == LegendShape::Line);
  CHECK(std::string(out[0].label_key) == "legend.grid_cu");
  CHECK(out[1].layer == OverlayLayer::MvL1);
  CHECK(out[1].color_index == 2);
  CHECK(out[1].rgba == kOverlayColors[2]);
  CHECK(out[1].shape == LegendShape::Arrow);
  CHECK(std::string(out[1].label_key) == "legend.mv_l1");
  CHECK(out[2].layer == OverlayLayer::MvL0);
  CHECK(out[2].color_index == 1);
  CHECK(out[2].rgba == kOverlayColors[1]);
  CHECK(out[2].shape == LegendShape::Arrow);
  CHECK(std::string(out[2].label_key) == "legend.mv_l0");

  // The colour-blind palette gives the same entries with the CVD colours.
  std::array<LegendEntry, 8> cvd{};
  REQUIRE(ovt_render_legend(cfg, true, cvd.data(), cvd.size()) == 3);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(cvd.at(i).layer == out.at(i).layer);
    CHECK(cvd.at(i).rgba == kOverlayColorsCvd.at(cvd.at(i).color_index));
  }
  CHECK(cvd[2].rgba == 0xE69F00FFU);
}

TEST_CASE("render_test_overlay_legend: capacity, ordering and every layer", "[fast][render][overlay]") {
  OverlayConfig cfg;
  std::array<LegendEntry, 40> out{};
  CHECK(ovt_render_legend(cfg, false, out.data(), out.size()) == 0);  // no layers

  cfg.layers = 0x3FFFFU;  // all 18 layer bits
  const size_t n = ovt_render_legend(cfg, false, nullptr, 0);
  REQUIRE(n == 33);
  // A short buffer is filled up to its capacity and the full count is returned.
  std::array<LegendEntry, 2> small{};
  CHECK(ovt_render_legend(cfg, false, small.data(), small.size()) == n);
  CHECK(small[0].layer == OverlayLayer::Heatmap);
  CHECK(small[0].color_index == ovt::render::kLegendNoColor);
  CHECK(small[0].shape == LegendShape::Gradient);
  CHECK(small[1].layer == OverlayLayer::PredFill);

  REQUIRE(ovt_render_legend(cfg, false, out.data(), out.size()) == n);
  std::set<uint32_t> layers_seen;
  std::set<std::string> keys;
  for (size_t i = 0; i < n; ++i) {
    const LegendEntry& e = out.at(i);
    layers_seen.insert(bit(e.layer));
    keys.insert(e.label_key);
    if (e.color_index != ovt::render::kLegendNoColor) {
      const uint32_t expected = e.shape == LegendShape::Fill ? ovt::render::overlay_fill_rgba(e.color_index, false)
                                                             : kOverlayColors.at(e.color_index);
      CHECK(e.rgba == expected);
    }
  }
  CHECK(layers_seen.size() == 18);
  CHECK(keys.size() == n);  // every entry has its own translation key

  // Draw order (§10.5.6): fills before the skip hatch, grids TU → CTU, arrows L1 → L0 → bi, labels before the probe.
  const auto pos = [&](OverlayLayer l) {
    for (size_t i = 0; i < n; ++i) {
      if (out.at(i).layer == l) {
        return i;
      }
    }
    return n;
  };
  CHECK(pos(OverlayLayer::RefIdxFill) < pos(OverlayLayer::SkipHatch));
  CHECK(pos(OverlayLayer::GridTU) < pos(OverlayLayer::GridPU));
  CHECK(pos(OverlayLayer::GridPU) < pos(OverlayLayer::GridCU));
  CHECK(pos(OverlayLayer::GridCU) < pos(OverlayLayer::GridCTU));
  CHECK(pos(OverlayLayer::GridCTU) < pos(OverlayLayer::GridPartition));
  CHECK(pos(OverlayLayer::DeblockEdges) < pos(OverlayLayer::IntraDirs));
  CHECK(pos(OverlayLayer::MvL1) < pos(OverlayLayer::MvL0));
  CHECK(pos(OverlayLayer::MvL0) < pos(OverlayLayer::MvBi));
  CHECK(pos(OverlayLayer::Selection) < pos(OverlayLayer::Labels));
  CHECK(pos(OverlayLayer::Labels) < pos(OverlayLayer::Probe));

  // PredFill swatches use the fill alpha.
  cfg.layers = bit(OverlayLayer::PredFill);
  REQUIRE(ovt_render_legend(cfg, false, out.data(), out.size()) == 7);
  CHECK(out[1].color_index == 1);
  CHECK(out[1].rgba == 0xFF851B4DU);
}
