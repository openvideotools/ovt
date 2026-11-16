// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay colour tables and legend data (chapter 10 §10.5.2, WI-2.439). Render-library symbols, not part of the core
// C API of chapter 12 §12.3: the GUI links libovt-render and includes this header directly (chapter 11 §11.3.3,
// §11.4.5). Included by overlay_set.h, so OverlayLayer and OverlayConfig are only forward-declared here.

#ifndef OVT_RENDER_OVERLAY_COLORS_H
#define OVT_RENDER_OVERLAY_COLORS_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "ovt/render/render_api.h"

namespace ovt::render {

enum class OverlayLayer : uint32_t;  // overlay_set.h
struct OverlayConfig;                // overlay_set.h

// RGBA (straight alpha), index = row of the §10.5.2 table. Rows with a line/arrow alpha and a fill alpha (1-3 and 18)
// carry the line/arrow alpha here; overlay_fill_rgba() gives the fill variant.
OVT_RENDER_API extern const std::array<uint32_t, 33> kOverlayColors;
// Colour-blind-safe variant; same indices. Only the four coded roles differ (L0, L1, bi, intra: rows 0-3 and 20).
// Okabe-Ito per §10.5.2; row 20 (intra direction stroke) takes the intra colour too.
OVT_RENDER_API extern const std::array<uint32_t, 33> kOverlayColorsCvd;

// The halo rule of WI-2.439 AT2 (§10.5.2): RGBA of the 1-px halo drawn around each line/arrow/label colour of the
// matching palette (#000000 or #FFFFFF at the table alpha), 0 = no halo. Rows 15 and 19 carry the outer outline of the
// §10.5.2 table.
OVT_RENDER_API extern const std::array<uint32_t, 33> kOverlayHalo;
OVT_RENDER_API extern const std::array<uint32_t, 33> kOverlayHaloCvd;

// Swatch kind of a legend entry (§10.5.2, WI-2.439 step 2).
enum class LegendShape : uint8_t {
  Fill,      // tinted block
  Line,      // grid, boundary, edge, outline or crosshair stroke
  Arrow,     // motion vector
  Hatch,     // 45° skip hatch
  Gradient,  // heatmap: the GUI draws the LUT bar of OverlayConfig::lut
  Text       // block labels
};

inline constexpr uint8_t kLegendNoColor = 0xFF;  // LegendEntry::color_index of an entry without a palette row (heatmap)

struct LegendEntry {
  uint8_t color_index;    // row of the table above
  uint32_t rgba;          // from the active palette
  OverlayLayer layer;     // the layer bit this entry explains
  const char* label_key;  // translation key, e.g. "legend.inter_l0"; the GUI runs it through qsTr
  LegendShape shape;      // swatch kind (WI-2.439 step 2)
};

// The colour the fill passes draw for `index` (the fill alpha of §10.5.2 rows 1-3 and 18, else the table value);
// 0 for an index outside the table.
OVT_RENDER_API uint32_t overlay_fill_rgba(size_t index, bool cvd) noexcept;

}  // namespace ovt::render

/// Legend data for the GUI (chapter 11 §11.4.5). Fills `out` (capacity `cap`) with one entry per layer enabled in
/// `cfg.layers`, in draw order (§10.5.6), from `kOverlayColorsCvd` when `cvd` is true; returns the number of entries
/// needed (may exceed `cap`; call again with a larger buffer). `noexcept`; never allocates. A layer drawn in several
/// colours (PredFill, RefIdxFill, DeblockEdges, Selection) gets one entry per colour.
OVT_RENDER_API size_t ovt_render_legend(const ovt::render::OverlayConfig& cfg, bool cvd, ovt::render::LegendEntry* out,
                                        size_t cap) noexcept;

#endif  // OVT_RENDER_OVERLAY_COLORS_H
