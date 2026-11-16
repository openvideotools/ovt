// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 tile_group_obu(sz) (chapter 7 §7.3.6 row 29; WI-1.118 part c): the tile range, the tile sizes and the
// opaque tile_data[TileNum] byte ranges. Tile data itself is not decoded.

#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecTileGroup = "AV1 5.11.1";
constexpr std::string_view kDiagTileSizeOverrun = "SYN-AV1-0020";
constexpr std::string_view kDiagForbiddenValue = "SYN-GEN-0003";

[[noreturn]] void overrun(uint64_t tileSize, uint64_t left, uint64_t bit_offset) {
  throw ParseError{std::string{kDiagTileSizeOverrun},
                   fmt::format("tile size {} overruns tile group ({} bytes left)", tileSize, left), bit_offset};
}

// tg_start / tg_end (AV1 §5.11.1). f(tileBits) can code a tile past the last one when TileCols or TileRows is not a
// power of two; such a range is not a tile group of this frame.
void read_tile_range(SyntaxBuilder& b, const TileInfo& ti, TileGroup& tg, uint64_t start_bit) {
  tg.tile_start_and_end_present_flag = ti.NumTiles > 1 && b.read_flag("tile_start_and_end_present_flag");
  if (ti.NumTiles <= 1 || !tg.tile_start_and_end_present_flag) {
    tg.tg_start = 0;
    tg.tg_end = ti.NumTiles == 0 ? 0 : ti.NumTiles - 1;
    return;
  }
  const unsigned tileBits = static_cast<unsigned>(ti.TileColsLog2) + ti.TileRowsLog2;
  tg.tg_start = static_cast<uint32_t>(b.read_u("tg_start", tileBits));
  tg.tg_end = static_cast<uint32_t>(b.read_u("tg_end", tileBits));
  // SPEC-ASSUMPTION(#546): AV1 §6.10.1 requires tg_start <= tg_end < NumTiles; chapter 7 registers no code, so the
  // out-of-range group is SYN-GEN-0003 (forbidden value) and stops the parse like an overrun.
  if (tg.tg_end >= ti.NumTiles || tg.tg_start > tg.tg_end) {
    throw ParseError{std::string{kDiagForbiddenValue},
                     fmt::format("tg_start {} / tg_end {}: forbidden/reserved value (NumTiles = {})", tg.tg_start,
                                 tg.tg_end, ti.NumTiles),
                     start_bit};
  }
}

}  // namespace

TileGroup parse_tile_group_obu(BitReader& br, SyntaxBuilder& b, const TileInfo& ti, uint32_t sz) {
  // AV1 §5.11.1
  TileGroup tg;
  const auto start = br.mark();
  auto s = b.scoped("tile_group_obu", kSpecTileGroup);
  b.derived("NumTiles", ti.NumTiles);
  read_tile_range(b, ti, tg, start);
  br.byte_alignment();
  tg.headerBytes = static_cast<uint32_t>((br.bit_pos() - start) / 8);
  if (tg.headerBytes > sz) {
    overrun(tg.headerBytes, sz, start);
  }
  uint64_t left = sz - tg.headerBytes;
  for (uint32_t TileNum = tg.tg_start; TileNum <= tg.tg_end && ti.TileCols != 0; ++TileNum) {
    TileGroupTile t;
    t.TileNum = TileNum;
    t.tileRow = TileNum / ti.TileCols;
    t.tileCol = TileNum % ti.TileCols;
    const bool lastTile = TileNum == tg.tg_end;
    b.begin_array_element("tile", static_cast<int>(TileNum), kSpecTileGroup);
    if (lastTile) {
      t.tileSize = left;
    } else {
      if (left < ti.TileSizeBytes) {
        overrun(ti.TileSizeBytes, left, br.bit_pos());
      }
      const auto size_pos = br.bit_pos();
      t.tile_size_minus_1 = static_cast<uint32_t>(b.read_le("tile_size_minus_1", ti.TileSizeBytes));
      t.tileSize = uint64_t{t.tile_size_minus_1} + 1;
      left -= ti.TileSizeBytes;
      if (t.tileSize > left) {
        overrun(t.tileSize, left, size_pos);
      }
      left -= t.tileSize;
    }
    // tile_data(): opaque (chapter 7 §7.3.6 row 29); the reader must hold the bytes too.
    if (t.tileSize * 8 > br.bits_left()) {
      overrun(t.tileSize, br.bits_left() / 8, br.bit_pos());
    }
    t.MiRowStart = ti.MiRowStarts[t.tileRow];
    t.MiRowEnd = ti.MiRowStarts[t.tileRow + 1];
    t.MiColStart = ti.MiColStarts[t.tileCol];
    t.MiColEnd = ti.MiColStarts[t.tileCol + 1];
    t.byte_offset = br.bit_pos() / 8;
    (void)b.read_bytes("tile_data", static_cast<size_t>(t.tileSize));
    b.derived("tile byte range", fmt::format("tile byte range = [{}, {}) tileRow {} tileCol {}", t.byte_offset,
                                             t.byte_offset + t.tileSize, t.tileRow, t.tileCol));
    b.end_array_element();
    tg.tiles.push_back(t);
  }
  tg.frame_complete = ti.NumTiles != 0 && tg.tg_end == ti.NumTiles - 1;
  if (tg.frame_complete) {
    b.derived("frame_complete", "tg_end == NumTiles - 1: the frame is complete (SeenFrameHeader = 0)");
  }
  tg.span = br.span_since(start);
  return tg;
}

}  // namespace ovt::syntax::av1
