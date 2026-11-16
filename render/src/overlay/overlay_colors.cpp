// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay colour tables, halo rule and legend data (chapter 10 §10.5.2, §10.5.6; WI-2.439).

#include "ovt/render/overlay_colors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ovt/render/overlay_set.h"

namespace ovt::render {

// clang-format off
const std::array<uint32_t, 33> kOverlayColors = {
    0x2ECC4066U, 0xFF851BFFU, 0xB10DC9FFU, 0x00E5FFFFU, 0xAAAAAA99U, 0x20C99766U, 0xFFDC0066U, 0xF012BE66U,  //  0- 7
    0xFFFFFFE6U, 0xFFFF00CCU, 0x00FFFFB3U, 0xFF00FF99U, 0xFF4136FFU, 0x7FDBFFCCU, 0x0074D9FFU, 0xFFFFFFFFU,  //  8-15
    0xFFD700FFU, 0xFF4136FFU, 0x39CCCCFFU, 0xFFFFFFFFU, 0x2ECC40FFU, 0xFFDC00FFU,                            // 16-21
    0xE6194B66U, 0x3CB44B66U, 0xFFE11966U, 0x4363D866U, 0xF5823166U, 0x911EB466U, 0x46F0F066U, 0xF032E666U,  // 22-29
    0xE53935FFU, 0x1E88E5FFU, 0x43A047FFU,                                                                   // 30-32
};

// Okabe-Ito: intra #009E73 (rows 0 and 20), L0 #E69F00, L1 #56B4E9, bi #0072B2, §10.5.2.
const std::array<uint32_t, 33> kOverlayColorsCvd = {
    0x009E7366U, 0xE69F00FFU, 0x56B4E9FFU, 0x0072B2FFU, 0xAAAAAA99U, 0x20C99766U, 0xFFDC0066U, 0xF012BE66U,  //  0- 7
    0xFFFFFFE6U, 0xFFFF00CCU, 0x00FFFFB3U, 0xFF00FF99U, 0xFF4136FFU, 0x7FDBFFCCU, 0x0074D9FFU, 0xFFFFFFFFU,  //  8-15
    0xFFD700FFU, 0xFF4136FFU, 0x39CCCCFFU, 0xFFFFFFFFU, 0x009E73FFU, 0xFFDC00FFU,                            // 16-21
    0xE6194B66U, 0x3CB44B66U, 0xFFE11966U, 0x4363D866U, 0xF5823166U, 0x911EB466U, 0x46F0F066U, 0xF032E666U,  // 22-29
    0xE53935FFU, 0x1E88E5FFU, 0x43A047FFU,                                                                   // 30-32
};

// Every line/arrow/label colour (rows 1, 2, 3, 8-21) that composites to under 3:1 WCAG contrast against the dark or the
// light background of render/tests/test_overlay_contrast.cpp gets a #000000 halo (all of them are light colours, so
// black is the contrasting one). Row 19 keeps the 0xCC alpha of its SDF outline. test_overlay_contrast.cpp checks it.
const std::array<uint32_t, 33> kOverlayHalo = {
    0U,          0x000000FFU, 0U,          0x000000FFU, 0U,          0U,          0U,          0U,           //  0- 7
    0x000000FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0U,          0x000000FFU,  //  8-15
    0x000000FFU, 0x000000FFU, 0x000000FFU, 0x000000CCU, 0x000000FFU, 0x000000FFU,                            // 16-21
    0U,          0U,          0U,          0U,          0U,          0U,          0U,          0U,           // 22-29
    0U,          0U,          0U,                                                                            // 30-32
};

const std::array<uint32_t, 33> kOverlayHaloCvd = {
    0U,          0x000000FFU, 0x000000FFU, 0U,          0U,          0U,          0U,          0U,           //  0- 7
    0x000000FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0U,          0x000000FFU,  //  8-15
    0x000000FFU, 0x000000FFU, 0x000000FFU, 0x000000CCU, 0x000000FFU, 0x000000FFU,                            // 16-21
    0U,          0U,          0U,          0U,          0U,          0U,          0U,          0U,           // 22-29
    0U,          0U,          0U,                                                                            // 30-32
};
// clang-format on

uint32_t overlay_fill_rgba(size_t index, bool cvd) noexcept {
  const auto& table = cvd ? kOverlayColorsCvd : kOverlayColors;
  if (index >= table.size()) {
    return 0;
  }
  const uint32_t rgb = table[index] & 0xFFFFFF00U;
  switch (index) {
    case 1:
    case 2:
    case 3:
      return rgb | 0x4DU;  // inter fills
    case 18:
      return rgb | 0x33U;  // selection rectangle fill
    default:
      return table[index];
  }
}

}  // namespace ovt::render

namespace {

using ovt::render::LegendShape;
using ovt::render::OverlayLayer;

struct LegendRow {
  OverlayLayer layer;
  uint8_t color_index;
  LegendShape shape;
  const char* label_key;
};

// Draw order of §10.5.6: heatmap; pred-mode / ref-idx fills; skip hatch; grids fine to coarse; slice/tile boundaries;
// deblock edges; intra strokes; MV arrows L1, L0, bi; hover then selection outline; labels; probe, selection rect.
constexpr std::array<LegendRow, 33> kLegendRows = {{
    {OverlayLayer::Heatmap, ovt::render::kLegendNoColor, LegendShape::Gradient, "legend.heatmap"},
    {OverlayLayer::PredFill, 0, LegendShape::Fill, "legend.intra"},
    {OverlayLayer::PredFill, 1, LegendShape::Fill, "legend.inter_l0"},
    {OverlayLayer::PredFill, 2, LegendShape::Fill, "legend.inter_l1"},
    {OverlayLayer::PredFill, 3, LegendShape::Fill, "legend.inter_bi"},
    {OverlayLayer::PredFill, 5, LegendShape::Fill, "legend.intra_bc"},
    {OverlayLayer::PredFill, 6, LegendShape::Fill, "legend.pcm"},
    {OverlayLayer::PredFill, 7, LegendShape::Fill, "legend.palette"},
    {OverlayLayer::RefIdxFill, 22, LegendShape::Fill, "legend.ref_slot_0"},
    {OverlayLayer::RefIdxFill, 23, LegendShape::Fill, "legend.ref_slot_1"},
    {OverlayLayer::RefIdxFill, 24, LegendShape::Fill, "legend.ref_slot_2"},
    {OverlayLayer::RefIdxFill, 25, LegendShape::Fill, "legend.ref_slot_3"},
    {OverlayLayer::RefIdxFill, 26, LegendShape::Fill, "legend.ref_slot_4"},
    {OverlayLayer::RefIdxFill, 27, LegendShape::Fill, "legend.ref_slot_5"},
    {OverlayLayer::RefIdxFill, 28, LegendShape::Fill, "legend.ref_slot_6"},
    {OverlayLayer::RefIdxFill, 29, LegendShape::Fill, "legend.ref_slot_7"},
    {OverlayLayer::SkipHatch, 4, LegendShape::Hatch, "legend.skip"},
    {OverlayLayer::GridTU, 11, LegendShape::Line, "legend.grid_tu"},
    {OverlayLayer::GridPU, 10, LegendShape::Line, "legend.grid_pu"},
    {OverlayLayer::GridCU, 9, LegendShape::Line, "legend.grid_cu"},
    {OverlayLayer::GridCTU, 8, LegendShape::Line, "legend.grid_ctu"},
    {OverlayLayer::GridPartition, 12, LegendShape::Line, "legend.slice_tile_boundary"},
    {OverlayLayer::DeblockEdges, 13, LegendShape::Line, "legend.deblock_bs1"},
    {OverlayLayer::DeblockEdges, 14, LegendShape::Line, "legend.deblock_bs2"},
    {OverlayLayer::IntraDirs, 20, LegendShape::Line, "legend.intra_direction"},
    {OverlayLayer::MvL1, 2, LegendShape::Arrow, "legend.mv_l1"},
    {OverlayLayer::MvL0, 1, LegendShape::Arrow, "legend.mv_l0"},
    {OverlayLayer::MvBi, 3, LegendShape::Arrow, "legend.mv_bi"},
    {OverlayLayer::Selection, 16, LegendShape::Line, "legend.hover"},
    {OverlayLayer::Selection, 15, LegendShape::Line, "legend.selection"},
    {OverlayLayer::Labels, 19, LegendShape::Text, "legend.labels"},
    {OverlayLayer::Probe, 17, LegendShape::Line, "legend.probe"},
    {OverlayLayer::SelectionRect, 18, LegendShape::Line, "legend.selection_rect"},
}};

}  // namespace

size_t ovt_render_legend(const ovt::render::OverlayConfig& cfg, bool cvd, ovt::render::LegendEntry* out,
                         size_t cap) noexcept {
  const std::span<ovt::render::LegendEntry> dst(out, out == nullptr ? 0 : cap);
  size_t n = 0;
  for (const LegendRow& row : kLegendRows) {
    if ((cfg.layers & static_cast<uint32_t>(row.layer)) == 0) {
      continue;
    }
    if (n < dst.size()) {
      uint32_t rgba = 0;
      if (row.color_index != ovt::render::kLegendNoColor) {
        rgba = row.shape == LegendShape::Fill
                   ? ovt::render::overlay_fill_rgba(row.color_index, cvd)
                   : (cvd ? ovt::render::kOverlayColorsCvd : ovt::render::kOverlayColors)[row.color_index];
      }
      dst[n] = {row.color_index, rgba, row.layer, row.label_key, row.shape};
    }
    ++n;
  }
  return n;
}
