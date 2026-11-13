// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay configuration and builder (chapter 10 §10.2.5, verbatim). OverlayBuilder and the colour tables of
// overlay_colors.h (§10.5.2) are implemented by their own work items (overlay passes; WI-2.439 for the colours, which
// adds the overlay_colors.h include here); WI-1.400 only declares the interface.

#ifndef OVT_RENDER_OVERLAY_SET_H
#define OVT_RENDER_OVERLAY_SET_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"  // VideoSurface, OverlaySet (forward declaration)
#include "ovt_model_generated.h"       // ovt::fb::FrameAnalysis, ovt::fb::Rect

namespace ovt::render {

enum class OverlayLayer : uint32_t {  // bitmask; draw order = ascending bit index (§10.5.6)
  Heatmap = 1U << 0,
  PredFill = 1U << 1,  // prediction-kind tint per block
  SkipHatch = 1U << 2,
  GridTU = 1U << 3,  // AV1: TXB/—/Block/SB; H.264: Tx/SubMB/—/MB (GridTU, GridPU, GridCU, GridCTU)
  GridPU = 1U << 4,
  GridCU = 1U << 5,
  GridCTU = 1U << 6,
  GridPartition = 1U << 7,  // slice / tile boundaries
  MvL0 = 1U << 8,
  MvL1 = 1U << 9,
  MvBi = 1U << 10,
  DeblockEdges = 1U << 11,
  IntraDirs = 1U << 12,   // short direction stroke per intra block
  RefIdxFill = 1U << 13,  // colour per ref slot
  Selection = 1U << 14,
  Labels = 1U << 15,
  Probe = 1U << 16,
  SelectionRect = 1U << 17
};

enum class HeatmapSource : uint8_t {
  None,
  QP,
  BitsX8,
  MvMagnitude,
  TxSize,
  FilterIdx,
  RefIdx,
  IntraMode,
  MetricMap /* from MetricsResult map blob */,
  Custom /* host-provided per-cell u16 */
};
enum class Lut : uint8_t { Viridis, Turbo, Cividis, Jet, Hot };
enum class LabelContent : uint8_t { None, BlockSize, PredMode, QP, Bits, MvValues, IntraMode, TxType, RefIdx, BlockId };

// NOLINTBEGIN(readability-isolate-declaration): verbatim from §10.2.5
struct OverlayConfig {
  uint32_t layers = 0;  // OverlayLayer bits
  HeatmapSource heatmap = HeatmapSource::None;
  Lut lut = Lut::Viridis;
  float heatmap_alpha = 0.6F;
  float heatmap_min = 0, heatmap_max = 0;  // 0,0 = auto (min/max over the frame); bits are log2-scaled when auto
  LabelContent label = LabelContent::None;
  float label_zoom_threshold = 24.0F;  // device px per cell below which labels are not drawn
  float mv_scale = 1.0F;               // arrow length multiplier (MVs are drawn in luma samples × mv_scale)
  float mv_min_px = 2.0F;              // arrows shorter than this on screen are drawn as 2×2 dots
  float line_px = 1.0F;                // grid line width in device pixels (scaled by device_pixel_ratio)
  uint32_t selected_block = 0xFFFFFFFFU;
  int32_t selected_mv = -1;
  ovt::fb::Rect selection_rect;    // zero w/h = none
  int probe_x = -1, probe_y = -1;  // image coordinates of the pixel probe; -1 none
};
// NOLINTEND(readability-isolate-declaration)

class OVT_RENDER_API OverlayBuilder {
 public:
  explicit OverlayBuilder(VideoSurface& owner);
  // Builds GPU data for one frame. fa = FrameAnalysis root (verified FlatBuffer). custom_cells (optional) is
  // cells_w*cells_h u16 values for HeatmapSource::Custom or MetricMap (host converts a metric map blob to the cell
  // grid before calling). Thread-safe for the CPU part; GPU upload happens inside set_overlays().
  std::unique_ptr<OverlaySet> build(const ovt::fb::FrameAnalysis* fa, const OverlayConfig& cfg,
                                    const uint16_t* custom_cells = nullptr);
  // WI-2.430 addition: the same for a FlatBuffer given as bytes, which is verified (flatbuffers::Verifier) first.
  // Returns nullptr when the buffer does not verify. Use this whenever the buffer is not already trusted: a root
  // pointer alone carries no length, so build(const FrameAnalysis*) can only check vector lengths against the grid.
  // SPEC-ASSUMPTION(#317): §10.2.5 says "verified FlatBuffer" but WI-2.430 step 1 verifies inside build().
  std::unique_ptr<OverlaySet> build(const uint8_t* buf, size_t size, const OverlayConfig& cfg,
                                    const uint16_t* custom_cells = nullptr);

 private:
  VideoSurface* owner_;
};

}  // namespace ovt::render

#endif  // OVT_RENDER_OVERLAY_SET_H
