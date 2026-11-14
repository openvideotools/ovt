// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 tile_info() (chapter 7 §7.3.6 row 15; WI-1.117). The function mirrors AV1 §5.9.15 line by line, including
// the uniform and the explicit (non-uniform) spacing paths; the derived MiColStarts / MiRowStarts arrays and the
// tile counts are what the tile group parser (WI-1.118) and the grid (chapter 6) consume.

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecTileInfo = "AV1 5.9.15";

std::string starts_text(std::string_view name, const std::vector<uint32_t>& starts) {
  std::string out = fmt::format("{} = {{", name);
  for (size_t i = 0; i < starts.size(); ++i) {
    out += fmt::format("{}{}", i == 0 ? "" : ", ", starts[i]);
  }
  out += "}";
  return out;
}

// SPEC-ASSUMPTION(#513): AV1 §5.9.15 requires TileCols <= MAX_TILE_COLS and TileRows <= MAX_TILE_ROWS;
// §7.9 registers no code for the violation, SYN-AV1-0015 is proposed. The explicit spacing path can exceed the
// limits on wide frames (every tile at least one superblock), so the vectors are not capped.
void check_tile_count(SyntaxBuilder& b, std::string_view what, uint32_t count, uint32_t max) {
  if (count > max) {
    b.diag(Severity::Error, kDiagFrameHeaderRange, fmt::format("{} = {} exceeds {}", what, count, max), kSpecTileInfo);
  }
}

// The uniform_tile_spacing_flag == 1 path: increment_tile_cols_log2 / increment_tile_rows_log2 bits, then evenly
// spaced starts.
void parse_uniform_spacing(SyntaxBuilder& b, TileInfo& t, uint32_t MiCols, uint32_t MiRows) {
  uint32_t TileColsLog2 = t.minLog2TileCols;
  while (TileColsLog2 < t.maxLog2TileCols) {
    if (!b.read_flag("increment_tile_cols_log2")) {
      break;
    }
    ++TileColsLog2;
  }
  const uint32_t tileWidthSb = (t.sbCols + (1U << TileColsLog2) - 1U) >> TileColsLog2;
  t.MiColStarts.clear();
  for (uint32_t startSb = 0; startSb < t.sbCols; startSb += tileWidthSb) {
    t.MiColStarts.push_back(startSb << t.sbShift);
  }
  t.TileCols = static_cast<uint32_t>(t.MiColStarts.size());
  t.MiColStarts.push_back(MiCols);
  t.TileColsLog2 = static_cast<uint8_t>(TileColsLog2);

  const uint32_t minLog2TileRows = t.minLog2Tiles > TileColsLog2 ? t.minLog2Tiles - TileColsLog2 : 0U;
  uint32_t TileRowsLog2 = minLog2TileRows;
  while (TileRowsLog2 < t.maxLog2TileRows) {
    if (!b.read_flag("increment_tile_rows_log2")) {
      break;
    }
    ++TileRowsLog2;
  }
  const uint32_t tileHeightSb = (t.sbRows + (1U << TileRowsLog2) - 1U) >> TileRowsLog2;
  t.MiRowStarts.clear();
  for (uint32_t startSb = 0; startSb < t.sbRows; startSb += tileHeightSb) {
    t.MiRowStarts.push_back(startSb << t.sbShift);
  }
  t.TileRows = static_cast<uint32_t>(t.MiRowStarts.size());
  t.MiRowStarts.push_back(MiRows);
  t.TileRowsLog2 = static_cast<uint8_t>(TileRowsLog2);
}

// The uniform_tile_spacing_flag == 0 path: width_in_sbs_minus_1 / height_in_sbs_minus_1 per tile with ns().
void parse_explicit_spacing(SyntaxBuilder& b, TileInfo& t, uint32_t MiCols, uint32_t MiRows, uint32_t maxTileWidthSb) {
  t.widestTileSb = 0;
  t.MiColStarts.clear();
  uint32_t startSb = 0;
  for (int i = 0; startSb < t.sbCols; ++i) {
    t.MiColStarts.push_back(startSb << t.sbShift);
    const uint32_t maxWidth = std::min(t.sbCols - startSb, maxTileWidthSb);
    b.begin_array_element("width_in_sbs_minus_1", i, kSpecTileInfo);
    const uint32_t width_in_sbs_minus_1 = b.read_ns("width_in_sbs_minus_1", maxWidth);
    b.end_array_element();
    t.width_in_sbs_minus_1.push_back(width_in_sbs_minus_1);
    const uint32_t sizeSb = width_in_sbs_minus_1 + 1U;
    t.widestTileSb = std::max(sizeSb, t.widestTileSb);
    startSb += sizeSb;
  }
  t.TileCols = static_cast<uint32_t>(t.MiColStarts.size());
  t.MiColStarts.push_back(MiCols);
  t.TileColsLog2 = static_cast<uint8_t>(tile_log2(1, t.TileCols));
  check_tile_count(b, "TileCols", t.TileCols, kMaxTileCols);

  uint32_t maxTileAreaSb = t.sbRows * t.sbCols;
  if (t.minLog2Tiles > 0) {
    maxTileAreaSb = (t.sbRows * t.sbCols) >> (t.minLog2Tiles + 1U);
  }
  // widestTileSb is 0 only for an empty frame (sbCols == 0); the spec's division then has no meaning.
  const uint32_t maxTileHeightSb = std::max(maxTileAreaSb / std::max(t.widestTileSb, 1U), 1U);

  t.MiRowStarts.clear();
  startSb = 0;
  for (int i = 0; startSb < t.sbRows; ++i) {
    t.MiRowStarts.push_back(startSb << t.sbShift);
    const uint32_t maxHeight = std::min(t.sbRows - startSb, maxTileHeightSb);
    b.begin_array_element("height_in_sbs_minus_1", i, kSpecTileInfo);
    const uint32_t height_in_sbs_minus_1 = b.read_ns("height_in_sbs_minus_1", maxHeight);
    b.end_array_element();
    t.height_in_sbs_minus_1.push_back(height_in_sbs_minus_1);
    startSb += height_in_sbs_minus_1 + 1U;
  }
  t.TileRows = static_cast<uint32_t>(t.MiRowStarts.size());
  t.MiRowStarts.push_back(MiRows);
  t.TileRowsLog2 = static_cast<uint8_t>(tile_log2(1, t.TileRows));
  check_tile_count(b, "TileRows", t.TileRows, kMaxTileRows);
}

}  // namespace

uint32_t tile_log2(uint32_t blkSize, uint32_t target) noexcept {
  // AV1 §5.9.15 tile_log2(): k = 0; while ((blkSize << k) < target) k++. blkSize == 0 would never terminate;
  // every caller passes >= 1. The shift cannot overflow for the sizes the syntax allows (target <= 2^16).
  uint32_t k = 0;
  if (blkSize == 0) {
    return 0;
  }
  while (k < 31U && (static_cast<uint64_t>(blkSize) << k) < target) {
    ++k;
  }
  return k;
}

TileRect TileInfo::tile_rect(uint32_t tileRow, uint32_t tileCol) const noexcept {
  if (tileRow >= TileRows || tileCol >= TileCols || MiColStarts.size() <= tileCol + 1U ||
      MiRowStarts.size() <= tileRow + 1U) {
    return {};
  }
  TileRect r;
  r.x = MiColStarts[tileCol] * kMiSize;
  r.y = MiRowStarts[tileRow] * kMiSize;
  r.width = (MiColStarts[tileCol + 1U] - MiColStarts[tileCol]) * kMiSize;
  r.height = (MiRowStarts[tileRow + 1U] - MiRowStarts[tileRow]) * kMiSize;
  return r;
}

TileInfo parse_tile_info(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, uint32_t MiCols, uint32_t MiRows) {
  // AV1 §5.9.15
  TileInfo t;
  const auto start = br.mark();
  auto s = b.scoped("tile_info", kSpecTileInfo);
  if (seq.use_128x128_superblock) {
    t.sbCols = (MiCols + 31U) >> 5U;
    t.sbRows = (MiRows + 31U) >> 5U;
    t.sbShift = 5;
  } else {
    t.sbCols = (MiCols + 15U) >> 4U;
    t.sbRows = (MiRows + 15U) >> 4U;
    t.sbShift = 4;
  }
  const uint32_t sbSize = t.sbShift + 2U;
  const uint32_t maxTileWidthSb = kMaxTileWidth >> sbSize;
  const uint32_t maxTileAreaSb = kMaxTileArea >> (2U * sbSize);
  t.minLog2TileCols = tile_log2(maxTileWidthSb, t.sbCols);
  t.maxLog2TileCols = tile_log2(1, std::min(t.sbCols, kMaxTileCols));
  t.maxLog2TileRows = tile_log2(1, std::min(t.sbRows, kMaxTileRows));
  t.minLog2Tiles = std::max(t.minLog2TileCols, tile_log2(maxTileAreaSb, t.sbRows * t.sbCols));
  b.derived("superblocks",
            fmt::format("sbCols = {}, sbRows = {}, minLog2TileCols = {}, maxLog2TileCols = {}, "
                        "maxLog2TileRows = {}, minLog2Tiles = {}",
                        t.sbCols, t.sbRows, t.minLog2TileCols, t.maxLog2TileCols, t.maxLog2TileRows, t.minLog2Tiles));

  t.uniform_tile_spacing_flag = b.read_flag("uniform_tile_spacing_flag");
  if (t.uniform_tile_spacing_flag) {
    parse_uniform_spacing(b, t, MiCols, MiRows);
  } else {
    parse_explicit_spacing(b, t, MiCols, MiRows, maxTileWidthSb);
  }

  if (t.TileColsLog2 > 0 || t.TileRowsLog2 > 0) {
    t.context_update_tile_id = static_cast<uint32_t>(
        b.read_u("context_update_tile_id", static_cast<unsigned>(t.TileRowsLog2) + t.TileColsLog2));
    t.tile_size_bytes_minus_1 = static_cast<uint8_t>(b.read_u("tile_size_bytes_minus_1", 2));
    t.TileSizeBytes = static_cast<uint8_t>(t.tile_size_bytes_minus_1 + 1U);
  } else {
    t.context_update_tile_id = 0;
    t.tile_size_bytes_minus_1 = 3;  // TileSizeBytes stays 4: never read by tile_group_obu() with one tile
    t.TileSizeBytes = 4;
  }
  t.NumTiles = t.TileCols * t.TileRows;
  b.derived("tiles", fmt::format("TileCols = {}, TileRows = {}, TileColsLog2 = {}, TileRowsLog2 = {}, NumTiles = {}, "
                                 "TileSizeBytes = {}",
                                 t.TileCols, t.TileRows, t.TileColsLog2, t.TileRowsLog2, t.NumTiles, t.TileSizeBytes));
  b.derived("MiColStarts", starts_text("MiColStarts", t.MiColStarts));
  b.derived("MiRowStarts", starts_text("MiRowStarts", t.MiRowStarts));
  t.span = br.span_since(start);
  return t;
}

}  // namespace ovt::syntax::av1
