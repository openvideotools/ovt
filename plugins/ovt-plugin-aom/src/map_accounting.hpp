// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// libaom Accounting -> SymbolCost, symbol_names, Block.bits_x8 and CellGrid.bits_x8 (chapter 07 §7.6.1 accounting
// row, chapter 06 §6.3.4/§6.3.5, WI-2.157). Free of libaom and FlatBuffers headers: map_mi.cpp serialises the
// result, and the unit tests build AccountingData by hand.
#ifndef OVT_PLUGIN_AOM_MAP_ACCOUNTING_HPP
#define OVT_PLUGIN_AOM_MAP_ACCOUNTING_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "block_data.hpp"

namespace ovt::aom {

/// One aggregated SymbolCost: every record of the frame at accounting context (x4, y4) with this symbol id.
struct SymbolTotal {
  uint16_t x4 = 0;
  uint16_t y4 = 0;
  uint16_t symbol_id = 0;
  uint32_t bits_x8 = 0;
  uint32_t count = 0;
};

struct MappedAccounting {
  std::vector<uint32_t> block_bits;   // Block.bits_x8 per block id (all zero when there is no accounting)
  std::vector<uint32_t> cell_bits;    // CellGrid.bits_x8; empty when absent for this frame
  std::vector<SymbolTotal> symbols;   // FrameAnalysis.symbols, sorted by (y4, x4, symbol_id); in-grid contexts only
  std::vector<std::string> names;     // FrameAnalysis.symbol_names
  uint64_t frame_level_bits = 0;      // bits of the symbols attributed to the frame root itself
  bool last_tile_group_only = false;  // several tile groups: the accounting covers the last one only
};

/// Grid field bit 13 (bits_x8) of ovt_AnalysisCaps.grid_fields, announced when libaom records accounting.
constexpr uint32_t kGridFieldBits = 1U << 13U;

/// Maps the frame's accounting onto the block tree. @p parent holds Block.parent per block id in pre-order (so a
/// child's id is larger than its parent's; id 0 is the frame root), @p block_id the CellGrid.block_id vector of a
/// @p cells_w x @p cells_h grid. A symbol whose context lies in the grid is charged to the leaf block owning that
/// cell; a symbol with context -1 or outside the grid (a block in libaom's padding columns or rows) is charged to
/// the root only and is left out of `symbols`. Every block's bits_x8 = its own symbols + the sum of its
/// children's; each leaf's bits are spread over its cells, floor plus remainder in raster order (§6.3.4).
MappedAccounting map_accounting(const AccountingData& a, std::span<const int32_t> parent,
                                std::span<const uint32_t> block_id, uint32_t cells_w, uint32_t cells_h);

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_MAP_ACCOUNTING_HPP
