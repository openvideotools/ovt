// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Definition of the opaque OverlaySet (chapter 10 §10.2.5). Internal to libovt-render. WI-1.400 needs a complete
// type so that VideoSurface::set_overlays() can own one. WI-2.430 adds the CPU arrays OverlayBuilder fills from a
// FrameAnalysis (§10.5.1); the GPU buffers that mirror them arrive with WI-2.431 and the later overlay passes.

#ifndef OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H
#define OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H

#include <cstdint>
#include <string>
#include <vector>

#include "ovt/render/overlay_set.h"

namespace ovt::render {

// One axis-aligned partition boundary segment in luma samples (coded-picture coordinates), x0 <= x1 and y0 <= y1.
struct PartitionLine {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // NOLINT(readability-isolate-declaration)
  uint8_t kind = 0;                        // ovt::fb::PartitionKind of the first partition that owns the edge
  uint16_t partition = 0;                  // PartitionInfo.index of that partition
};

class OverlaySet {
 public:
  OverlaySet() = default;
  OverlaySet(const OverlaySet&) = delete;
  OverlaySet& operator=(const OverlaySet&) = delete;
  OverlaySet(OverlaySet&&) = delete;
  OverlaySet& operator=(OverlaySet&&) = delete;
  ~OverlaySet() = default;

  OverlayConfig config;               // the configuration the set was built for
  uint32_t cells_w = 0, cells_h = 0;  // NOLINT(readability-isolate-declaration): CellGrid size in 4x4 luma cells

  // Cell attribute texels, RGBA8UI, cells_w * cells_h * 4 bytes, row-major (§10.5.1): R = PredKind (0xFF when the
  // plugin gave no `pred`), G = ref0 + 1 and B = ref1 + 1 (0 = no reference, 0xFF = plugin gave no ref vector),
  // A = flags (bit0 skip, bit1 merge/direct, bit2 bi-predicted). Empty unless PredFill, SkipHatch or RefIdxFill is set.
  std::vector<uint8_t> cell_attributes;

  // Heatmap texels, R16_UNORM values normalised to [heat_lo, heat_hi] of the source domain, cells_w * cells_h entries.
  // heat_valid[i] == 0 marks "not applicable" cells (-1 / 0xFF in the CellGrid), which the pass draws with alpha 0.
  // Empty when config.heatmap == HeatmapSource::None.
  std::vector<uint16_t> heatmap;
  std::vector<uint8_t> heat_valid;
  float heat_lo = 0, heat_hi = 0;  // NOLINT(readability-isolate-declaration): the range actually applied

  // Slice and tile boundaries (§10.5.3), each edge once, in a deterministic order. Empty unless GridPartition is set.
  std::vector<PartitionLine> partition_lines;

  // Notes about input the builder could not use (a vector whose length differs from cells_w * cells_h, an
  // unusable grid). The build never fails on them: the affected data is left empty.
  std::vector<std::string> diagnostics;
};

}  // namespace ovt::render

#endif  // OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H
