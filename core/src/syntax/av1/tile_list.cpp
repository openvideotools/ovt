// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 tile_list_obu() (chapter 7 §7.3.6 row 30; WI-1.116).

#include "ovt/cxx/syntax/av1/tile_list.hpp"

#include <cstdint>

namespace ovt::syntax::av1 {

TileList parse_tile_list(BitReader& br, SyntaxBuilder& b) {
  TileList t;
  const auto start = br.mark();
  auto s = b.scoped("tile_list_obu", "AV1 5.12.1");
  t.output_frame_width_in_tiles_minus_1 = static_cast<uint8_t>(b.read_u("output_frame_width_in_tiles_minus_1", 8));
  t.output_frame_height_in_tiles_minus_1 = static_cast<uint8_t>(b.read_u("output_frame_height_in_tiles_minus_1", 8));
  t.tile_count_minus_1 = static_cast<uint16_t>(b.read_u("tile_count_minus_1", 16));
  // Every entry is at least 5 bytes, so the count is bounded by the OBU size: a short OBU ends in BR-0001 at
  // the first entry that does not fit (§7.3.1 rule 3 needs no extra check).
  t.entries.reserve(static_cast<size_t>(t.tile_count_minus_1) + 1);
  for (int i = 0; i <= t.tile_count_minus_1; ++i) {
    TileListEntry e;
    b.begin_array_element("tile_list_entry", i, "AV1 5.12.2");
    e.anchor_frame_idx = static_cast<uint8_t>(b.read_u("anchor_frame_idx", 8));
    e.anchor_tile_row = static_cast<uint8_t>(b.read_u("anchor_tile_row", 8));
    e.anchor_tile_col = static_cast<uint8_t>(b.read_u("anchor_tile_col", 8));
    e.tile_data_size_minus_1 = static_cast<uint16_t>(b.read_le("tile_data_size_minus_1", 2));
    const auto data_start = br.mark();
    // coded_tile_data is opaque (§7.3.1 rule 2); read_bytes copies at most 256 bytes into the node.
    (void)b.read_bytes("coded_tile_data", static_cast<size_t>(e.tile_data_size_minus_1) + 1);
    e.coded_tile_data = br.span_since(data_start);
    b.end_array_element();
    t.entries.push_back(e);
  }
  b.diag(Severity::Info, "SYN-AV1-0006", "large-scale tile list OBU: indexed but not analysed");
  t.span = br.span_since(start);
  return t;
}

}  // namespace ovt::syntax::av1
