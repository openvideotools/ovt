// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 tile_list_obu() (AV1 §5.12, large-scale tile decoding): chapter 7 §7.3.6 row 30, WI-1.116. C++ helper
// header (D22): semi-public, no ABI promise, never included from a C header, not installed.

#ifndef OVT_CXX_SYNTAX_AV1_TILE_LIST_HPP
#define OVT_CXX_SYNTAX_AV1_TILE_LIST_HPP

#include <cstdint>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::av1 {

/// tile_list_entry() (AV1 §5.12.2).
struct TileListEntry {
  uint8_t anchor_frame_idx = 0;
  uint8_t anchor_tile_row = 0;
  uint8_t anchor_tile_col = 0;
  uint16_t tile_data_size_minus_1 = 0;
  BitSpan coded_tile_data{0, 0};  // the opaque tile bytes, tile_data_size_minus_1 + 1 of them
};

/// §7.3.6 row 30: tile_list_obu().
struct TileList {
  uint8_t output_frame_width_in_tiles_minus_1 = 0;
  uint8_t output_frame_height_in_tiles_minus_1 = 0;
  uint16_t tile_count_minus_1 = 0;
  std::vector<TileListEntry> entries;  // tile_count_minus_1 + 1 entries
  BitSpan span{0, 0};
};

/// Parses tile_list_obu() at the current position (node "tile_list_obu"); the OBU payload ends with the last
/// entry (no trailing_bits, AV1 §5.3.1). Each entry's coded_tile_data is an opaque Bytes node without a copy.
/// Diagnostic Info SYN-AV1-0006 records that large-scale tile streams are indexed but not analysed
/// (SPEC-ASSUMPTION(#321)). A tile_data_size that overruns the OBU throws BR-0001.
[[nodiscard]] TileList parse_tile_list(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::av1

#endif  // OVT_CXX_SYNTAX_AV1_TILE_LIST_HPP
