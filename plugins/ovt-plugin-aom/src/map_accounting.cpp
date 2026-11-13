// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// libaom Accounting -> SymbolCost, symbol_names and bits_x8 (chapter 07 §7.6.1 accounting row, WI-2.157).

#include "map_accounting.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace ovt::aom {

namespace {

uint32_t saturate(uint64_t v) {
  return static_cast<uint32_t>(std::min<uint64_t>(v, std::numeric_limits<uint32_t>::max()));
}

}  // namespace

MappedAccounting map_accounting(const AccountingData& a, std::span<const int32_t> parent,
                                std::span<const uint32_t> block_id, uint32_t cells_w, uint32_t cells_h) {
  MappedAccounting out;
  const size_t n_blocks = parent.size();
  out.block_bits.assign(n_blocks, 0);
  const size_t n_cells = size_t{cells_w} * cells_h;
  if (!a.valid || n_blocks == 0 || block_id.size() != n_cells) {
    return out;
  }
  out.names = a.names;
  out.last_tile_group_only = a.tile_groups > 1;

  // Own bits per block, and the per-(y4, x4, id) totals. The symbols keep the context libaom recorded them with:
  // some are read before the next block sets its context (loop restoration units, for one) and stay with the
  // previous block, as in libaom's inspect output (§7.6.1 WI-2.157 notes).
  std::vector<uint64_t> total(n_blocks, 0);
  std::map<std::tuple<uint16_t, uint16_t, uint32_t>, SymbolTotal> agg;
  for (const AcctSymbol& s : a.symbols) {
    const bool in_grid = s.x >= 0 && s.y >= 0 && static_cast<uint32_t>(s.x) < cells_w &&
                         static_cast<uint32_t>(s.y) < cells_h && s.id < a.names.size();
    if (!in_grid) {
      // SPEC-ASSUMPTION(#408): "outside the frame" is outside the cell grid; such records are charged to the root
      // and left out of symbols[], whose x4/y4 V-024 confines to the grid.
      total[0] += s.bits;
      out.frame_level_bits += s.bits;
      continue;
    }
    const auto x4 = static_cast<uint16_t>(s.x);
    const auto y4 = static_cast<uint16_t>(s.y);
    const uint32_t owner = block_id[size_t{y4} * cells_w + x4];
    total[owner < n_blocks ? owner : 0] += s.bits;
    SymbolTotal& t = agg[{y4, x4, s.id}];
    t.x4 = x4;
    t.y4 = y4;
    t.symbol_id = static_cast<uint16_t>(s.id);
    t.bits_x8 = saturate(uint64_t{t.bits_x8} + s.bits);
    t.count = saturate(uint64_t{t.count} + s.samples);
  }
  out.symbols.reserve(agg.size());
  for (const auto& [key, t] : agg) {
    out.symbols.push_back(t);  // std::map order is (y4, x4, id): the canonical order of §6.3.7
  }

  // Pre-order ids: every child comes after its parent, so one backward pass sums each subtree.
  for (size_t id = n_blocks; id-- > 1;) {
    const int32_t p = parent[id];
    if (p >= 0 && static_cast<size_t>(p) < id) {
      total[static_cast<size_t>(p)] += total[id];
    }
  }
  for (size_t id = 0; id < n_blocks; ++id) {
    out.block_bits[id] = saturate(total[id]);
  }

  // Cells: the owning block's bits, floor plus remainder over its cells in raster order (§6.3.4, rule V-020). A
  // frame whose accounting covers only its last tile group has no cell bits (announced absent, §7.6.1).
  // SPEC-ASSUMPTION(#408): Block.bits_x8 stays (earlier tile groups' blocks read 0 = unknown); only the cells go.
  if (out.last_tile_group_only) {
    return out;
  }
  std::vector<uint32_t> cells_of(n_blocks, 0);
  for (const uint32_t id : block_id) {
    if (id < n_blocks) {
      ++cells_of[id];
    }
  }
  std::vector<uint32_t> seen(n_blocks, 0);
  out.cell_bits.assign(n_cells, 0);
  for (size_t i = 0; i < n_cells; ++i) {
    const uint32_t id = block_id[i];
    if (id == 0 || id >= n_blocks) {
      continue;  // a cell no leaf covers (map_blocks warns): the root's bits are not spread
    }
    const uint32_t bits = out.block_bits[id];
    const uint32_t count = cells_of[id];
    out.cell_bits[i] = bits / count + (seen[id] < bits % count ? 1U : 0U);
    ++seen[id];
  }
  return out;
}

}  // namespace ovt::aom
