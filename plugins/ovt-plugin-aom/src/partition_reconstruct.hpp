// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Partition tree of an AV1 superblock rebuilt from the per-MI block sizes of libaom's inspection data (chapter 07
// §7.6.1 bsize row and WI-2.156 notes). Stock libaom stores the block size in every MI of a block but neither marks
// block origins nor records the partition symbol, so each square node is classified by the block size at its
// origin and, where two partitions give the same origin size, by the size at one quadrant origin. Free of libaom
// headers.
#ifndef OVT_PLUGIN_AOM_PARTITION_RECONSTRUCT_HPP
#define OVT_PLUGIN_AOM_PARTITION_RECONSTRUCT_HPP

#include <cstdint>
#include <span>
#include <vector>

namespace ovt::aom {

/// One node of a superblock's partition tree. A leaf (partition == 0, PARTITION_NONE) is a coded block; any
/// other node is a square that was partitioned, with 2, 3 or 4 children (or fewer at the frame edge, where children
/// whose origin lies outside the MI grid are not coded).
struct PartNode {
  int32_t parent = -1;             // index into the output vector; -1 for the superblock
  std::vector<uint32_t> children;  // indices into the output vector
  int32_t mi_row = 0;
  int32_t mi_col = 0;
  int32_t mi_w = 0;  // size in MI (4x4) units: the leaf's block size, or the square of a partitioned node
  int32_t mi_h = 0;
  uint8_t bsize = 0;       // BLOCK_SIZE of mi_w x mi_h
  uint8_t partition = 0;   // Av1Partition read at this node; 0 for leaves
  uint8_t level = 0;       // depth below the superblock (superblock = 0)
  bool ambiguous = false;  // the origin sizes fit no partition type; resolved as PARTITION_SPLIT
};

/// The block sizes of the frame's MI grid: bsize[mi_row * mi_cols + mi_col] (libaom's mi_rows x mi_cols, padding
/// included).
struct BsizeGrid {
  std::span<const uint8_t> bsize;
  int32_t mi_rows = 0;
  int32_t mi_cols = 0;
};

/// Appends the partition tree of the superblock of @p sb_mi x @p sb_mi MIs (16 for 64x64, 32 for 128x128) at
/// (@p mi_row, @p mi_col), which must lie inside the grid, to @p out in pre-order (parents before children,
/// children in coding order). Every MI of the grid inside the superblock is covered by exactly one leaf, whatever
/// the grid holds: an inconsistent grid yields PARTITION_SPLIT nodes flagged `ambiguous` down to 4x4 leaves.
/// Returns the index of the superblock node.
uint32_t reconstruct_superblock(const BsizeGrid& grid, int32_t mi_row, int32_t mi_col, int32_t sb_mi,
                                std::vector<PartNode>* out);

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_PARTITION_RECONSTRUCT_HPP
