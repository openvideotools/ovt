// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reference writer for `tiny64_av1.inspect.json` (WI-0.093 -> WI-0.090): the chapter 06 §6.8.3 export of
// tiny64_av1.fa in the byte format of libaom v3.12.1 examples/inspect.c (framing, key order, the *Map
// objects copied from its map_entry tables, no RLE, no accounting because the fixture has no `symbols`).
// The production exporter is WI-0.090's; this file exists so that its golden is written from the rules and
// never by hand. Test-only code, deliberately independent of any model helper.
//
// SPEC-ASSUMPTION(#164): intra blocks have no interp_filter or motion_mode in the model; both are written as
// 0 (EIGHTTAP / SIMPLE_TRANSLATION), what libaom's zero-initialised MB_MODE_INFO holds. The file ends with
// `]` and a newline (`inspect` prints no newline after `]`; the repository's end-of-file hook requires one).

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fixtures.hpp"
#include "fixtures_common.hpp"

namespace ovt::golden {
namespace {

using MapEntries = std::vector<std::pair<std::string_view, int>>;

// Names whose values are 0, 1, 2, ... in order (the libaom enums below are contiguous).
MapEntries sequence(std::initializer_list<std::string_view> names) {
  MapEntries map;
  int value = 0;
  for (const auto name : names) {
    map.emplace_back(name, value++);
  }
  return map;
}

const MapEntries& block_size_map() {
  static const MapEntries map =
      sequence({"BLOCK_4X4",   "BLOCK_4X8",    "BLOCK_8X4",    "BLOCK_8X8",     "BLOCK_8X16",  "BLOCK_16X8",
                "BLOCK_16X16", "BLOCK_16X32",  "BLOCK_32X16",  "BLOCK_32X32",   "BLOCK_32X64", "BLOCK_64X32",
                "BLOCK_64X64", "BLOCK_64X128", "BLOCK_128X64", "BLOCK_128X128", "BLOCK_4X16",  "BLOCK_16X4",
                "BLOCK_8X32",  "BLOCK_32X8",   "BLOCK_16X64",  "BLOCK_64X16"});
  return map;
}
const MapEntries& tx_size_map() {
  static const MapEntries map = sequence({"TX_4X4", "TX_8X8", "TX_16X16", "TX_32X32", "TX_64X64", "TX_4X8", "TX_8X4",
                                          "TX_8X16", "TX_16X8", "TX_16X32", "TX_32X16", "TX_32X64", "TX_64X32",
                                          "TX_4X16", "TX_16X4", "TX_8X32", "TX_32X8", "TX_16X64", "TX_64X16"});
  return map;
}
const MapEntries& tx_type_map() {
  static const MapEntries map = sequence({"DCT_DCT", "ADST_DCT", "DCT_ADST", "ADST_ADST", "FLIPADST_DCT",
                                          "DCT_FLIPADST", "FLIPADST_FLIPADST", "ADST_FLIPADST", "FLIPADST_ADST", "IDTX",
                                          "V_DCT", "H_DCT", "V_ADST", "H_ADST", "V_FLIPADST", "H_FLIPADST"});
  return map;
}
const MapEntries& dual_filter_map() {
  static const MapEntries map = sequence({"REG_REG", "REG_SMOOTH", "REG_SHARP", "SMOOTH_REG", "SMOOTH_SMOOTH",
                                          "SMOOTH_SHARP", "SHARP_REG", "SHARP_SMOOTH", "SHARP_SHARP"});
  return map;
}
const MapEntries& prediction_mode_map() {
  static const MapEntries map =
      sequence({"DC_PRED",     "V_PRED",        "H_PRED",        "D45_PRED",    "D135_PRED",     "D113_PRED",
                "D157_PRED",   "D203_PRED",     "D67_PRED",      "SMOOTH_PRED", "SMOOTH_V_PRED", "SMOOTH_H_PRED",
                "PAETH_PRED",  "NEARESTMV",     "NEARMV",        "GLOBALMV",    "NEWMV",         "NEAREST_NEARESTMV",
                "NEAR_NEARMV", "NEAREST_NEWMV", "NEW_NEARESTMV", "NEAR_NEWMV",  "NEW_NEARMV",    "GLOBAL_GLOBALMV",
                "NEW_NEWMV",   "INTRA_INVALID"});
  return map;
}
const MapEntries& uv_prediction_mode_map() {
  static const MapEntries map =
      sequence({"UV_DC_PRED", "UV_V_PRED", "UV_H_PRED", "UV_D45_PRED", "UV_D135_PRED", "UV_D113_PRED", "UV_D157_PRED",
                "UV_D203_PRED", "UV_D67_PRED", "UV_SMOOTH_PRED", "UV_SMOOTH_V_PRED", "UV_SMOOTH_H_PRED",
                "UV_PAETH_PRED", "UV_CFL_PRED", "UV_MODE_INVALID"});
  return map;
}
const MapEntries& motion_mode_map() {
  static const MapEntries map = sequence({"SIMPLE_TRANSLATION", "OBMC_CAUSAL", "WARPED_CAUSAL"});
  return map;
}
const MapEntries& compound_type_map() {
  static const MapEntries map{{"COMPOUND_AVERAGE", 0}, {"COMPOUND_WEDGE", 2}, {"COMPOUND_DIFFWTD", 3}};
  return map;
}
const MapEntries& skip_map() {
  static const MapEntries map{{"SKIP", 1}, {"NO_SKIP", 0}};
  return map;
}
const MapEntries& intrabc_map() {
  static const MapEntries map{{"INTRABC", 1}, {"NO_INTRABC", 0}};
  return map;
}
const MapEntries& palette_map() {
  static const MapEntries map{{"ZERO_COLORS", 0}, {"TWO_COLORS", 2}, {"THREE_COLORS", 3}, {"FOUR_COLORS", 4},
                              {"FIVE_COLORS", 5}, {"SIX_COLORS", 6}, {"SEVEN_COLORS", 7}, {"EIGHT_COLORS", 8}};
  return map;
}
const MapEntries& refs_map() {
  static const MapEntries map = sequence({"INTRA_FRAME", "LAST_FRAME", "LAST2_FRAME", "LAST3_FRAME", "GOLDEN_FRAME",
                                          "BWDREF_FRAME", "ALTREF2_FRAME", "ALTREF_FRAME"});
  return map;
}
const MapEntries& config_map() {
  static const MapEntries map{{"MI_SIZE", 4}};
  return map;
}

std::string map_object(const MapEntries& map) {
  std::string out = "{";
  for (std::size_t i = 0; i < map.size(); ++i) {
    out += (i != 0 ? "," : "") + std::string("\"") + std::string(map[i].first) + "\":" + std::to_string(map[i].second);
  }
  return out + "}";
}

// `  "key": value,` followed by a newline: one frame-level member of the object.
std::string member(std::string_view key, const std::string& value) {
  return "  \"" + std::string(key) + "\": " + value + ",\n";
}

std::string json_string(const std::string& text) {
  return '"' + text + '"';
}

// What the layers read for one cell.
struct CellValues {
  int block_size = 0;
  int tx_size = 0;
  int tx_type = 0;
  int dual_filter = 0;
  int mode = 0;
  int uv_mode = 0;
  int motion_mode = 0;
  int compound_type = 0;
  int skip = 0;
  std::array<int, 2> filter{};
  int cdef_level = 0;
  int cdef_strength = 0;
  int cfl_alpha_idx = 0;
  int cfl_alpha_sign = 0;
  int delta_q = 0;
  int seg_id = 0;
  std::array<int, 4> mv{};  // col, row, col, row in 1/8 luma sample
  int intrabc = 0;
  int palette = 0;
  int uv_palette = 0;
  std::array<int, 2> ref_frame{};
};

int libaom_compound_type(fb::Av1CompoundType type) {
  switch (type) {
    case fb::Av1CompoundType::COMPOUND_WEDGE:
      return 2;
    case fb::Av1CompoundType::COMPOUND_DIFFWTD:
      return 3;
    case fb::Av1CompoundType::COMPOUND_DISTANCE:
      return 1;
    default:
      return 0;  // AVERAGE, INTRA and Unknown
  }
}

const fb::Av1TxbT* txb_covering(const fb::FrameAnalysisT& fa, const fb::BlockT& block, int x, int y,
                                const std::map<std::uint32_t, fb::Av1TxbT>& txbs) {
  for (const auto child : block.children) {
    const auto& node = *fa.blocks[child];
    if (node.kind == fb::BlockKind::TXB && x >= node.rect->x() && x < node.rect->x() + node.rect->w() &&
        y >= node.rect->y() && y < node.rect->y() + node.rect->h()) {
      return &txbs.at(child);
    }
  }
  return nullptr;
}

template <class T>
T unpack_ext(const std::vector<std::uint8_t>& ext) {
  T native;
  flatbuffers::GetRoot<typename T::TableType>(ext.data())->UnPackTo(&native);
  return native;
}

std::string layer(const char* name, const MapEntries* map, const std::vector<CellValues>& cells, std::size_t width,
                  int length, int (*get)(const CellValues&, int)) {
  std::string out;
  if (map != nullptr) {
    out += "  \"" + std::string(name) + "Map\": " + map_object(*map) + ",\n";
  }
  out += "  \"" + std::string(name) + "\": [";
  const std::size_t rows = cells.size() / width;
  for (std::size_t r = 0; r < rows; ++r) {
    out += "[";
    for (std::size_t c = 0; c < width; ++c) {
      const CellValues& v = cells[(r * width) + c];
      if (length == 0) {
        out += std::to_string(get(v, 0));
      } else {
        out += "[";
        for (int i = 0; i < length; ++i) {
          out += (i != 0 ? "," : "") + std::to_string(get(v, i));
        }
        out += "]";
      }
      out += c + 1 < width ? "," : "";
    }
    out += r + 1 < rows ? "]," : "]";
  }
  return out + "],\n";
}

}  // namespace

std::string build_tiny64_av1_inspect_json(const Bytes& av1) {
  fb::FrameAnalysisT fa;
  flatbuffers::GetRoot<fb::FrameAnalysis>(av1.data())->UnPackTo(&fa);

  std::map<std::uint32_t, fb::Av1BlockT> blocks;
  std::map<std::uint32_t, fb::Av1TxbT> txbs;
  for (const auto& b : fa.blocks) {
    if (b->ext_type == "Av1Block") {
      blocks[b->id] = unpack_ext<fb::Av1BlockT>(b->ext);
    } else if (b->ext_type == "Av1Txb") {
      txbs[b->id] = unpack_ext<fb::Av1TxbT>(b->ext);
    }
  }
  const auto filter = unpack_ext<fb::Av1FilterParamsT>(fa.filter_params);
  const auto state = unpack_ext<fb::Av1RefStateT>(fa.frame->ref_state);

  const auto cells_w = static_cast<int>(fa.grid->cells_w);
  const auto cells_h = static_cast<int>(fa.grid->cells_h);
  std::vector<CellValues> cells(static_cast<std::size_t>(cells_w) * static_cast<std::size_t>(cells_h));
  for (int y4 = 0; y4 < cells_h; ++y4) {
    for (int x4 = 0; x4 < cells_w; ++x4) {
      const auto i = (static_cast<std::size_t>(y4) * static_cast<std::size_t>(cells_w)) + static_cast<std::size_t>(x4);
      const fb::BlockT& node = *fa.blocks[fa.grid->block_id[i]];
      const fb::Av1BlockT& b = blocks.at(node.id);
      const fb::Av1TxbT* txb = txb_covering(fa, node, x4 * 4, y4 * 4, txbs);
      CellValues& v = cells[i];
      const bool intra = b.YMode < fb::Av1YMode::NEARESTMV;
      v.block_size = static_cast<int>(b.bsize);
      v.tx_size =
          (!intra || b.use_intrabc == 1) && txb != nullptr ? static_cast<int>(txb->txSz) : static_cast<int>(b.TxSize);
      v.tx_type = b.skip == 1 && txb != nullptr ? static_cast<int>(txb->TxType) : 0;  // ifd_inspect() quirk
      if (b.interp_filter.size() == 2) {
        v.filter = {static_cast<int>(b.interp_filter[0]), static_cast<int>(b.interp_filter[1])};
      }
      v.dual_filter = (v.filter[0] * 3) + v.filter[1];
      v.mode = static_cast<int>(b.YMode);
      v.uv_mode = intra ? static_cast<int>(b.UVMode) : 14;
      v.motion_mode = b.motion_mode == fb::Av1MotionMode::Av1MotionMode_Unknown ? 0 : static_cast<int>(b.motion_mode);
      v.compound_type = libaom_compound_type(b.compound_type);
      v.skip = b.skip == 1 ? 1 : 0;
      if (filter.enable_cdef != 0 && b.cdef_idx >= 0) {
        const auto idx = static_cast<std::size_t>(b.cdef_idx);
        v.cdef_level = filter.cdef->cdef_y_pri_strength[idx];
        const int sec = filter.cdef->cdef_y_sec_strength[idx];
        v.cdef_strength = sec == 3 ? 4 : sec;
      }
      if (intra && b.UVMode == 13) {
        v.cfl_alpha_idx = ((std::abs(b.CflAlphaU) - 1) << 4) + (std::abs(b.CflAlphaV) - 1);
        v.cfl_alpha_sign = b.cfl_alpha_signs;
      }
      v.delta_q = b.CurrentQIndex;
      v.seg_id = b.segment_id;
      for (std::size_t k = 0; k < b.Mv.size() && k < 4; k += 2) {
        v.mv[k] = b.Mv[k + 1];  // col first (inspect.c), Av1Block.Mv stores row first
        v.mv[k + 1] = b.Mv[k];
      }
      v.intrabc = b.use_intrabc == 1 ? 1 : 0;
      v.palette = b.PaletteSizeY;
      v.uv_palette = b.PaletteSizeUV;
      for (std::size_t k = 0; k < 2 && k < b.RefFrame.size(); ++k) {
        v.ref_frame[k] = b.RefFrame[k];
      }
    }
  }

  const auto width = static_cast<std::size_t>(cells_w);
  std::string out = "[\n{\n";
  out += layer("blockSize", &block_size_map(), cells, width, 0, [](const CellValues& v, int) { return v.block_size; });
  out += layer("transformSize", &tx_size_map(), cells, width, 0, [](const CellValues& v, int) { return v.tx_size; });
  out += layer("transformType", &tx_type_map(), cells, width, 0, [](const CellValues& v, int) { return v.tx_type; });
  out += layer("dualFilterType", &dual_filter_map(), cells, width, 0,
               [](const CellValues& v, int) { return v.dual_filter; });
  out += layer("mode", &prediction_mode_map(), cells, width, 0, [](const CellValues& v, int) { return v.mode; });
  out +=
      layer("uv_mode", &uv_prediction_mode_map(), cells, width, 0, [](const CellValues& v, int) { return v.uv_mode; });
  out +=
      layer("motion_mode", &motion_mode_map(), cells, width, 0, [](const CellValues& v, int) { return v.motion_mode; });
  out += layer("compound_type", &compound_type_map(), cells, width, 0,
               [](const CellValues& v, int) { return v.compound_type; });
  out += layer("skip", &skip_map(), cells, width, 0, [](const CellValues& v, int) { return v.skip; });
  out += layer("filter", nullptr, cells, width, 2,
               [](const CellValues& v, int k) { return v.filter[static_cast<std::size_t>(k)]; });
  out += layer("cdef_level", nullptr, cells, width, 0, [](const CellValues& v, int) { return v.cdef_level; });
  out += layer("cdef_strength", nullptr, cells, width, 0, [](const CellValues& v, int) { return v.cdef_strength; });
  out += layer("cfl_alpha_idx", nullptr, cells, width, 0, [](const CellValues& v, int) { return v.cfl_alpha_idx; });
  out += layer("cfl_alpha_sign", nullptr, cells, width, 0, [](const CellValues& v, int) { return v.cfl_alpha_sign; });
  out += layer("delta_q", nullptr, cells, width, 0, [](const CellValues& v, int) { return v.delta_q; });
  out += layer("seg_id", nullptr, cells, width, 0, [](const CellValues& v, int) { return v.seg_id; });
  out += layer("motionVectors", nullptr, cells, width, 4,
               [](const CellValues& v, int k) { return v.mv[static_cast<std::size_t>(k)]; });
  out += layer("intrabc", &intrabc_map(), cells, width, 0, [](const CellValues& v, int) { return v.intrabc; });
  out += layer("palette", &palette_map(), cells, width, 0, [](const CellValues& v, int) { return v.palette; });
  out += layer("uv_palette", &palette_map(), cells, width, 0, [](const CellValues& v, int) { return v.uv_palette; });
  out += layer("referenceFrame", &refs_map(), cells, width, 2,
               [](const CellValues& v, int k) { return v.ref_frame[static_cast<std::size_t>(k)]; });

  // First tile (tile_row = tile_col = 0), in MI units, clipped to the coded size (§6.8.3).
  const fb::PartitionInfoT* first_tile = nullptr;
  for (const auto& p : fa.partitions) {
    if (p->kind == fb::PartitionKind::Tile && p->tile_row == 0 && p->tile_col == 0) {
      first_tile = p.get();
    }
  }
  const int tile_cols = std::min(first_tile->rect->w(), fa.frame->coded_size->w()) / 4;
  const int tile_rows = std::min(first_tile->rect->h(), fa.frame->coded_size->h()) / 4;
  out += member("frame", "0");
  out += member("showFrame", std::to_string(fa.frame->shown ? 1 : 0));
  out += member("frameType", std::to_string(static_cast<int>(state.frame_type)));
  out += member("baseQIndex", std::to_string(filter.base_q_idx));
  out += member("tileCols", std::to_string(tile_cols));
  out += member("tileRows", std::to_string(tile_rows));
  out += member("deltaQPresentFlag", std::to_string(filter.delta_q_present));
  out += member("deltaQRes", std::to_string(filter.delta_q_res));
  out += member("config", map_object(config_map()));
  const std::string config_string =
      "ovt 0.0.0; plugin " + fa.plugin_id + " " + fa.plugin_version + "; decode_index_offset 0";
  out += "  \"configString\": " + json_string(config_string) + "\n";
  out += "},\nnull\n]\n";
  return out;
}

}  // namespace ovt::golden
