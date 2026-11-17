// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Definition of the opaque OverlaySet (chapter 10 §10.2.5). Internal to libovt-render. WI-1.400 needs a complete
// type so that VideoSurface::set_overlays() can own one. WI-2.430 adds the CPU arrays OverlayBuilder fills from a
// FrameAnalysis (§10.5.1); the GPU buffers that mirror them arrive with WI-2.431 and the later overlay passes.

#ifndef OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H
#define OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "overlay/arrow_instances.h"
#include "ovt/render/overlay_set.h"

namespace ovt::render {

// One axis-aligned partition boundary segment in luma samples (coded-picture coordinates), x0 <= x1 and y0 <= y1.
struct PartitionLine {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // NOLINT(readability-isolate-declaration)
  uint8_t kind = 0;                        // ovt::fb::PartitionKind of the first partition that owns the edge
  uint16_t partition = 0;                  // PartitionInfo.index of that partition
};

// Grid level slots of §10.5.3, in OverlayLayer bit order (GridTU = 1u << 3 ... GridCTU = 1u << 6).
enum class GridLevel : uint8_t { TU = 0, PU = 1, CU = 2, CTU = 3, None = 0xFF };

// Block tree node for the grid line pass (§10.5.3; the FrameAnalysis does not outlive build()): rect clipped to the
// coded picture (luma samples), children grid_nodes[first_child .. first_child + child_count).
struct GridNode {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // NOLINT(readability-isolate-declaration)
  uint32_t first_child = 0;
  uint32_t child_count = 0;
  GridLevel level = GridLevel::None;
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

  // CellGrid.block_id (cells_w * cells_h, 0xFFFFFFFF = no block): pick()'s CPU cell lookup and the id-only fill pass
  // (§10.5.6, §10.5.7; WI-2.435). Filled whatever the layers, so picking works with every overlay hidden; empty when
  // the grid has no block_id column of that length.
  std::vector<uint32_t> cell_block_id;

  // Slice and tile boundaries (§10.5.3), each edge once, in a deterministic order. Empty unless GridPartition is set.
  std::vector<PartitionLine> partition_lines;

  // Grid block tree, node 0 = blocks[0]; empty unless a GridTU..GridCTU layer is set. grid_min_side: smallest
  // unclipped block side per level (R-GRID-1), INT32_MAX when the level has no block.
  std::vector<GridNode> grid_nodes;
  std::array<int32_t, 4> grid_min_side{std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(),
                                       std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()};
  int32_t picture_w = 0, picture_h = 0;  // NOLINT(readability-isolate-declaration): coded size, luma samples

  // Deblocking edge classes per cell, RG8UI texels, cells_w * cells_h * 2 bytes (WI-2.436, §10.5.6 pass 7): R = the
  // cell's left edge, G = its top edge; 0 = not filtered or unknown, 1 = weak (§10.5.2 row 13), 2 = strong (row 14),
  // classified from CellGrid.deblock_bs_v/h by deblock_edge_class(). Empty unless DeblockEdges is set.
  std::vector<uint8_t> deblock_edges;

  // Rects (x0, y0, x1, y1, luma samples) of FrameAnalysis.blocks[i] and FrameAnalysis.mvs[i] for the selection
  // outline (WI-2.436, §10.5.6 pass 10). Filled whatever the layers: selected_block and selected_mv are draw-time
  // uniforms, not part of what a cached set was built for (WI-2.437). An entry with x1 <= x0 is not drawable.
  std::vector<std::array<int32_t, 4>> block_rects;
  std::vector<std::array<int32_t, 4>> mv_rects;

  // MV arrow and intra direction stroke instances (§10.5.1, §10.5.4; WI-2.434). Empty unless MvL0, MvL1, MvBi or
  // IntraDirs is set.
  detail::ArrowInstances arrows;

  // Notes about input the builder could not use (a vector whose length differs from cells_w * cells_h, an
  // unusable grid). The build never fails on them: the affected data is left empty.
  std::vector<std::string> diagnostics;
};

namespace detail {

// The CPU side of OverlayBuilder::build (§10.11), which never touches the surface: the fuzz target of
// core/tests/fuzz/fuzz_overlay_builder.cpp (issue #649) calls it without a VideoSurface. The buffer overload verifies
// the untrusted FlatBuffer first and returns nullptr when it is rejected.
std::unique_ptr<OverlaySet> build_overlay_set(const ovt::fb::FrameAnalysis* fa, const OverlayConfig& cfg,
                                              const uint16_t* custom_cells);
std::unique_ptr<OverlaySet> build_overlay_set(const uint8_t* buf, size_t size, const OverlayConfig& cfg,
                                              const uint16_t* custom_cells);

// WI-2.436: the §10.5.2 class of one CellGrid.deblock_bs_v/h value (chapter 6 §6.3: H.264 bS 0..4, HEVC bS 0..2, AV1
// filter size 0/4/6/8/16, 0xFF unknown): 0 = nothing drawn, 1 = weak (row 13), 2 = strong (row 14).
// SPEC-ASSUMPTION(#745): §10.5.1 names only "Bs 0 none, 1 weak, 2 strong"; the edges that touch an intra block
// are strong (H.264 bS 3 and 4, HEVC bS 2) and so is AV1's longest luma filter (16); everything else filtered is weak.
OVT_RENDER_API uint8_t deblock_edge_class(IntraCodec codec, uint8_t value);

}  // namespace detail

}  // namespace ovt::render

#endif  // OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H
