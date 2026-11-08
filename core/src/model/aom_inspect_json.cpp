// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libaom `inspect`-compatible JSON exporter (chapter 06 §6.8.3, WI-0.090). A streaming writer (no DOM): the
// per-cell values are gathered into one array per frame (step 1 of the WI, one Av1Block lookup per block, cached),
// then written layer by layer in the key order and framing of libaom v3.12.1 examples/inspect.c. The `*Map`
// objects carry exactly the names and values of inspect.c's map_entry tables at that pin.
//
// SPEC-ASSUMPTION(#164): intra blocks carry no interp_filter or motion_mode in the model; both are written as 0
// (EIGHTTAP / SIMPLE_TRANSLATION), what libaom's zero-initialised MB_MODE_INFO holds. The file ends with `]` and
// a newline (`inspect` prints none after `]`; the repository's end-of-file hook requires one).

#include "ovt/cxx/model/aom_inspect_json.hpp"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ovt::model {
namespace {

// ---- inspect.c map_entry tables (libaom v3.12.1) ---------------------------------------------------------

struct MapEntry {
  std::string_view name;
  int value;
};

constexpr std::array<MapEntry, 22> kBlockSizeMap{
    {{"BLOCK_4X4", 0},      {"BLOCK_4X8", 1},    {"BLOCK_8X4", 2},    {"BLOCK_8X8", 3},     {"BLOCK_8X16", 4},
     {"BLOCK_16X8", 5},     {"BLOCK_16X16", 6},  {"BLOCK_16X32", 7},  {"BLOCK_32X16", 8},   {"BLOCK_32X32", 9},
     {"BLOCK_32X64", 10},   {"BLOCK_64X32", 11}, {"BLOCK_64X64", 12}, {"BLOCK_64X128", 13}, {"BLOCK_128X64", 14},
     {"BLOCK_128X128", 15}, {"BLOCK_4X16", 16},  {"BLOCK_16X4", 17},  {"BLOCK_8X32", 18},   {"BLOCK_32X8", 19},
     {"BLOCK_16X64", 20},   {"BLOCK_64X16", 21}}};

constexpr std::array<MapEntry, 19> kTxSizeMap{{{"TX_4X4", 0},
                                               {"TX_8X8", 1},
                                               {"TX_16X16", 2},
                                               {"TX_32X32", 3},
                                               {"TX_64X64", 4},
                                               {"TX_4X8", 5},
                                               {"TX_8X4", 6},
                                               {"TX_8X16", 7},
                                               {"TX_16X8", 8},
                                               {"TX_16X32", 9},
                                               {"TX_32X16", 10},
                                               {"TX_32X64", 11},
                                               {"TX_64X32", 12},
                                               {"TX_4X16", 13},
                                               {"TX_16X4", 14},
                                               {"TX_8X32", 15},
                                               {"TX_32X8", 16},
                                               {"TX_16X64", 17},
                                               {"TX_64X16", 18}}};

constexpr std::array<MapEntry, 16> kTxTypeMap{{{"DCT_DCT", 0},
                                               {"ADST_DCT", 1},
                                               {"DCT_ADST", 2},
                                               {"ADST_ADST", 3},
                                               {"FLIPADST_DCT", 4},
                                               {"DCT_FLIPADST", 5},
                                               {"FLIPADST_FLIPADST", 6},
                                               {"ADST_FLIPADST", 7},
                                               {"FLIPADST_ADST", 8},
                                               {"IDTX", 9},
                                               {"V_DCT", 10},
                                               {"H_DCT", 11},
                                               {"V_ADST", 12},
                                               {"H_ADST", 13},
                                               {"V_FLIPADST", 14},
                                               {"H_FLIPADST", 15}}};

constexpr std::array<MapEntry, 9> kDualFilterMap{{{"REG_REG", 0},
                                                  {"REG_SMOOTH", 1},
                                                  {"REG_SHARP", 2},
                                                  {"SMOOTH_REG", 3},
                                                  {"SMOOTH_SMOOTH", 4},
                                                  {"SMOOTH_SHARP", 5},
                                                  {"SHARP_REG", 6},
                                                  {"SHARP_SMOOTH", 7},
                                                  {"SHARP_SHARP", 8}}};

constexpr std::array<MapEntry, 26> kPredictionModeMap{
    {{"DC_PRED", 0},      {"V_PRED", 1},         {"H_PRED", 2},
     {"D45_PRED", 3},     {"D135_PRED", 4},      {"D113_PRED", 5},
     {"D157_PRED", 6},    {"D203_PRED", 7},      {"D67_PRED", 8},
     {"SMOOTH_PRED", 9},  {"SMOOTH_V_PRED", 10}, {"SMOOTH_H_PRED", 11},
     {"PAETH_PRED", 12},  {"NEARESTMV", 13},     {"NEARMV", 14},
     {"GLOBALMV", 15},    {"NEWMV", 16},         {"NEAREST_NEARESTMV", 17},
     {"NEAR_NEARMV", 18}, {"NEAREST_NEWMV", 19}, {"NEW_NEARESTMV", 20},
     {"NEAR_NEWMV", 21},  {"NEW_NEARMV", 22},    {"GLOBAL_GLOBALMV", 23},
     {"NEW_NEWMV", 24},   {"INTRA_INVALID", 25}}};

constexpr std::array<MapEntry, 15> kUvPredictionModeMap{{{"UV_DC_PRED", 0},
                                                         {"UV_V_PRED", 1},
                                                         {"UV_H_PRED", 2},
                                                         {"UV_D45_PRED", 3},
                                                         {"UV_D135_PRED", 4},
                                                         {"UV_D113_PRED", 5},
                                                         {"UV_D157_PRED", 6},
                                                         {"UV_D203_PRED", 7},
                                                         {"UV_D67_PRED", 8},
                                                         {"UV_SMOOTH_PRED", 9},
                                                         {"UV_SMOOTH_V_PRED", 10},
                                                         {"UV_SMOOTH_H_PRED", 11},
                                                         {"UV_PAETH_PRED", 12},
                                                         {"UV_CFL_PRED", 13},
                                                         {"UV_MODE_INVALID", 14}}};

constexpr std::array<MapEntry, 3> kMotionModeMap{{{"SIMPLE_TRANSLATION", 0}, {"OBMC_CAUSAL", 1}, {"WARPED_CAUSAL", 2}}};

constexpr std::array<MapEntry, 3> kCompoundTypeMap{
    {{"COMPOUND_AVERAGE", 0}, {"COMPOUND_WEDGE", 2}, {"COMPOUND_DIFFWTD", 3}}};

constexpr std::array<MapEntry, 2> kSkipMap{{{"SKIP", 1}, {"NO_SKIP", 0}}};

constexpr std::array<MapEntry, 2> kIntrabcMap{{{"INTRABC", 1}, {"NO_INTRABC", 0}}};

constexpr std::array<MapEntry, 8> kPaletteMap{{{"ZERO_COLORS", 0},
                                               {"TWO_COLORS", 2},
                                               {"THREE_COLORS", 3},
                                               {"FOUR_COLORS", 4},
                                               {"FIVE_COLORS", 5},
                                               {"SIX_COLORS", 6},
                                               {"SEVEN_COLORS", 7},
                                               {"EIGHT_COLORS", 8}}};

constexpr std::array<MapEntry, 8> kRefsMap{{{"INTRA_FRAME", 0},
                                            {"LAST_FRAME", 1},
                                            {"LAST2_FRAME", 2},
                                            {"LAST3_FRAME", 3},
                                            {"GOLDEN_FRAME", 4},
                                            {"BWDREF_FRAME", 5},
                                            {"ALTREF2_FRAME", 6},
                                            {"ALTREF_FRAME", 7}}};

constexpr std::array<MapEntry, 1> kConfigMap{{{"MI_SIZE", 4}}};

// libaom enum values the layers need.
constexpr int kUvCflPred = 13;
constexpr int kUvModeInvalid = 14;
constexpr int kIntraModes = 13;  // YMode < 13 is an intra mode (Av1YMode NEARESTMV = 13)
constexpr int kCdefSecStrengthCoded3 = 3;
constexpr int kCdefSecStrength3Means = 4;
constexpr int kMaxInterpFilter = 3;  // BILINEAR; SWITCHABLE and Unknown never reach a block

// ---- per-cell values ---------------------------------------------------------------------------------------

// What the layers read for one cell. Everything except the transform fields is a property of the owning block.
struct Cell {
  int block_size = 0;
  int tx_size = 0;
  int tx_type = 0;
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
  std::array<int, 4> mv{};  // [Mv[0].col, Mv[0].row, Mv[1].col, Mv[1].row] in 1/8 luma sample (inspect.c order)
  int intrabc = 0;
  int palette = 0;
  int uv_palette = 0;
  std::array<int, 2> ref_frame{0, -1};
};

// A luma transform block of a leaf block: its rect and the two values the transform layers take from it.
struct LumaTxb {
  fb::Rect rect;
  int tx_size;
  int tx_type;
};

struct BlockValues {
  Cell cell;          // transform fields hold the block-level fallbacks
  bool use_txb_size;  // inter or IntraBC: transformSize comes from the covering TXB (inter_tx_size[])
  std::vector<LumaTxb> txbs;
};

template <class Table>
const Table* nested_root(const flatbuffers::Vector<std::uint8_t>* bytes) {
  if (bytes == nullptr || bytes->size() == 0) {
    return nullptr;
  }
  flatbuffers::Verifier verifier(bytes->data(), bytes->size());
  if (!verifier.VerifyBuffer<Table>(nullptr)) {
    return nullptr;
  }
  return flatbuffers::GetRoot<Table>(bytes->data());
}

// Av1CompoundType (AV1 spec numbering) -> libaom COMPOUND_TYPE (§6.8.3; inverse of the plugin remap, §7.6.1).
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

int interp_filter(fb::Av1InterpFilter filter) {
  const int value = static_cast<int>(filter);
  return value <= kMaxInterpFilter ? value : 0;
}

bool contains(const fb::Rect& rect, int x, int y) {
  return x >= rect.x() && x < rect.x() + rect.w() && y >= rect.y() && y < rect.y() + rect.h();
}

// cdef_level / cdef_strength of the block's 64x64 (§6.8.3); both stay 0 when CDEF is off.
void cdef_values(const fb::Av1Block& b, const fb::Av1FilterParams* filter, Cell& v) {
  // NOLINTNEXTLINE(bugprone-signed-char-misuse,cert-str34-c): a number (-1 = no CDEF index), not a character
  const int cdef_idx = b.cdef_idx();
  if (filter == nullptr || filter->enable_cdef() == 0 || filter->cdef() == nullptr || cdef_idx < 0) {
    return;
  }
  const auto idx = static_cast<flatbuffers::uoffset_t>(cdef_idx);
  const auto* pri = filter->cdef()->cdef_y_pri_strength();
  const auto* sec = filter->cdef()->cdef_y_sec_strength();
  if (pri != nullptr && idx < pri->size()) {
    v.cdef_level = pri->Get(idx);
  }
  if (sec != nullptr && idx < sec->size()) {
    const int strength = sec->Get(idx);
    v.cdef_strength = strength == kCdefSecStrengthCoded3 ? kCdefSecStrength3Means : strength;
  }
}

// motionVectors and referenceFrame (§6.8.3).
void motion_values(const fb::Av1Block& b, Cell& v) {
  if (const auto* mv = b.Mv(); mv != nullptr) {
    for (flatbuffers::uoffset_t k = 0; k + 1 < mv->size() && k < 4; k += 2) {
      v.mv[k] = mv->Get(k + 1);  // col first (inspect.c); Av1Block.Mv stores row first, already in 1/8 sample
      v.mv[k + 1] = mv->Get(k);
    }
  }
  if (const auto* refs = b.RefFrame(); refs != nullptr) {
    // RefFrame is a signed vector on purpose: -1 is NONE.
    for (flatbuffers::uoffset_t k = 0; k < 2 && k < refs->size(); ++k) {
      v.ref_frame[k] = refs->Get(k);  // NOLINT(bugprone-signed-char-misuse,cert-str34-c): a number, not a character
    }
  }
}

// The luma transform blocks among a block's children, for the transform layers.
// SPEC-ASSUMPTION(#269): only plane-0 TXBs count (libaom's inter_tx_size[] and tx_type_map are luma).
std::vector<LumaTxb> luma_txbs(const fb::FrameAnalysis& fa, const fb::Block& node) {
  std::vector<LumaTxb> txbs;
  const auto* children = node.children();
  if (children == nullptr) {
    return txbs;
  }
  for (const auto child_id : *children) {
    if (child_id >= fa.blocks()->size()) {
      continue;
    }
    const auto* child = fa.blocks()->Get(child_id);
    if (child->kind() != fb::BlockKind::TXB || child->rect() == nullptr) {
      continue;
    }
    const auto* txb = nested_root<fb::Av1Txb>(child->ext());
    if (txb != nullptr && txb->plane() == 0) {
      txbs.push_back({*child->rect(), static_cast<int>(txb->txSz()), static_cast<int>(txb->TxType())});
    }
  }
  return txbs;
}

BlockValues block_values(const fb::FrameAnalysis& fa, const fb::Block& node, const fb::Av1FilterParams* filter) {
  BlockValues values{};
  Cell& v = values.cell;
  const auto* b = nested_root<fb::Av1Block>(node.ext());
  if (b == nullptr) {
    return values;
  }
  const int y_mode = static_cast<int>(b->YMode());
  const bool intra = y_mode < kIntraModes;
  const bool intrabc = b->use_intrabc() == 1;
  values.use_txb_size = !intra || intrabc;

  v.block_size = static_cast<int>(b->bsize());
  v.tx_size = static_cast<int>(b->TxSize());
  v.tx_type = 0;
  v.mode = y_mode;
  v.uv_mode = intra ? static_cast<int>(b->UVMode()) : kUvModeInvalid;
  v.motion_mode = b->motion_mode() == fb::Av1MotionMode::Av1MotionMode_Unknown ? 0 : static_cast<int>(b->motion_mode());
  v.compound_type = libaom_compound_type(b->compound_type());
  v.skip = b->skip() == 1 ? 1 : 0;
  if (const auto* filters = b->interp_filter(); filters != nullptr && filters->size() == 2) {
    v.filter = {interp_filter(filters->Get(0)), interp_filter(filters->Get(1))};
  }
  cdef_values(*b, filter, v);
  if (intra && b->UVMode() == kUvCflPred) {
    v.cfl_alpha_idx = ((std::abs(b->CflAlphaU()) - 1) << 4) + (std::abs(b->CflAlphaV()) - 1);
    v.cfl_alpha_sign = b->cfl_alpha_signs();
  }
  v.delta_q = b->CurrentQIndex();
  v.seg_id = b->segment_id();
  motion_values(*b, v);
  v.intrabc = intrabc ? 1 : 0;
  v.palette = b->PaletteSizeY();
  v.uv_palette = b->PaletteSizeUV();
  values.txbs = luma_txbs(fa, node);
  return values;
}

std::vector<Cell> build_cells(const fb::FrameAnalysis& fa, const fb::Av1FilterParams* filter) {
  const auto& grid = *fa.grid();
  const auto cells_w = static_cast<std::size_t>(grid.cells_w());
  const auto cells_h = static_cast<std::size_t>(grid.cells_h());
  const auto* block_ids = grid.block_id();
  const auto* blocks = fa.blocks();
  if (block_ids == nullptr || blocks == nullptr || block_ids->size() != cells_w * cells_h) {
    throw std::invalid_argument("ovt::model::write_aom_inspect_json: grid.block_id does not cover the cell grid");
  }

  std::vector<std::optional<BlockValues>> cache(blocks->size());
  std::vector<Cell> cells(cells_w * cells_h);
  for (std::size_t y4 = 0; y4 < cells_h; ++y4) {
    for (std::size_t x4 = 0; x4 < cells_w; ++x4) {
      const std::size_t i = (y4 * cells_w) + x4;
      const std::uint32_t id = block_ids->Get(static_cast<flatbuffers::uoffset_t>(i));
      if (id >= blocks->size()) {
        throw std::invalid_argument("ovt::model::write_aom_inspect_json: grid.block_id names no block");
      }
      auto& entry = cache[id];
      if (!entry) {
        entry = block_values(fa, *blocks->Get(id), filter);
      }
      Cell& cell = cells[i];
      cell = entry->cell;
      const auto x = static_cast<int>(x4 * 4);
      const auto y = static_cast<int>(y4 * 4);
      const auto covering = std::find_if(entry->txbs.begin(), entry->txbs.end(),
                                         [&](const LumaTxb& t) { return contains(t.rect, x, y); });
      if (covering != entry->txbs.end()) {
        if (entry->use_txb_size) {
          cell.tx_size = covering->tx_size;
        }
        if (cell.skip == 1) {
          cell.tx_type = covering->tx_type;  // ifd_inspect() quirk: only skipped blocks report the TXB's type
        }
      }
    }
  }
  return cells;
}

// ---- writer ----------------------------------------------------------------------------------------------

void write_json_string(std::ostream& out, std::string_view text) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  out << '"';
  for (const char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') {
      out << '\\' << ch;
    } else if (byte < 0x20) {
      out << "\\u00" << kHex[byte >> 4U] << kHex[byte & 0xFU];
    } else {
      out << ch;
    }
  }
  out << '"';
}

template <std::size_t N>
void write_map(std::ostream& out, const std::array<MapEntry, N>& map) {
  out << '{';
  for (std::size_t i = 0; i < N; ++i) {
    out << (i != 0 ? "," : "") << '"' << map[i].name << "\":" << map[i].value;
  }
  out << '}';
}

template <std::size_t N>
void write_map_member(std::ostream& out, std::string_view name, const std::array<MapEntry, N>& map) {
  out << "  \"" << name << "Map\": ";
  write_map(out, map);
  out << ",\n";
}

// One per-MI layer: `  "name": [[v,v,...],[...]],` and a newline; Get(cell) returns an int or a std::array.
template <class Get>
void write_layer(std::ostream& out, std::string_view name, const std::vector<Cell>& cells, std::size_t width, Get get) {
  out << "  \"" << name << "\": [";
  const std::size_t rows = width == 0 ? 0 : cells.size() / width;
  for (std::size_t r = 0; r < rows; ++r) {
    out << '[';
    for (std::size_t c = 0; c < width; ++c) {
      const auto value = get(cells[(r * width) + c]);
      if constexpr (std::is_same_v<decltype(value), const int>) {
        out << value;
      } else {
        out << '[';
        for (std::size_t k = 0; k < value.size(); ++k) {
          out << (k != 0 ? "," : "") << value[k];
        }
        out << ']';
      }
      out << (c + 1 < width ? "," : "");
    }
    out << (r + 1 < rows ? "]," : "]");
  }
  out << "],\n";
}

// "symbolsMap" and "symbols" (§6.8.3 accounting): a context marker [x, y] whenever the position changes, then
// one [symbol_id, bits_x8, count] triple per aggregated SymbolCost, in the record's canonical order.
void write_accounting(std::ostream& out, const fb::FrameAnalysis& fa) {
  out << "  \"symbolsMap\": [";
  if (const auto* names = fa.symbol_names(); names != nullptr) {
    for (flatbuffers::uoffset_t i = 0; i < names->size(); ++i) {
      out << (i != 0 ? "," : "");
      const auto* name = names->Get(i);
      write_json_string(out, name != nullptr ? name->string_view() : std::string_view{});
    }
  }
  out << "],\n";
  out << "  \"symbols\": [\n    ";
  const auto& symbols = *fa.symbols();
  int x = -1;
  int y = -1;
  for (flatbuffers::uoffset_t i = 0; i < symbols.size(); ++i) {
    const auto* s = symbols.Get(i);
    if (s->x4() != x || s->y4() != y) {
      x = s->x4();
      y = s->y4();
      out << '[' << x << ',' << y << "],";
    }
    out << '[' << s->symbol_id() << ',' << s->bits_x8() << ',' << s->count() << ']';
    out << (i + 1 < symbols.size() ? "," : "");
  }
  out << "],\n";
}

// Width/height of the first tile (tile_row = tile_col = 0) in MI units, clipped to the coded size (§6.8.3:
// libaom's tile_mi_cols/tile_mi_rows, not tile counts). Without a Tile partition the frame is one tile.
// SPEC-ASSUMPTION(#269): a size that is not a multiple of 4 rounds up, like cells_w/cells_h.
std::pair<int, int> first_tile_mi(const fb::FrameAnalysis& fa) {
  const auto* coded = fa.frame()->coded_size();
  int w = coded != nullptr ? coded->w() : 0;
  int h = coded != nullptr ? coded->h() : 0;
  if (const auto* partitions = fa.partitions(); partitions != nullptr) {
    for (const auto* p : *partitions) {
      if (p->kind() == fb::PartitionKind::Tile && p->tile_row() == 0 && p->tile_col() == 0 && p->rect() != nullptr) {
        w = std::min(p->rect()->w(), w);
        h = std::min(p->rect()->h(), h);
        break;
      }
    }
  }
  return {(w + 3) / 4, (h + 3) / 4};
}

void write_frame(std::ostream& out, const fb::FrameAnalysis& fa, int ordinal, const std::string& config_string) {
  if (fa.frame() == nullptr || fa.grid() == nullptr) {
    throw std::invalid_argument("ovt::model::write_aom_inspect_json: record without frame info or cell grid");
  }
  if (fa.blocks() == nullptr || fa.blocks()->size() < 2 || fa.blocks()->Get(1)->kind() != fb::BlockKind::SB) {
    throw std::invalid_argument("ovt::model::write_aom_inspect_json: not an AV1 record");
  }
  const auto* filter = flatbuffers::GetStringView(fa.filter_params_type()) == "Av1FilterParams"
                           ? nested_root<fb::Av1FilterParams>(fa.filter_params())
                           : nullptr;
  const auto* state = flatbuffers::GetStringView(fa.frame()->ref_state_type()) == "Av1RefState"
                          ? nested_root<fb::Av1RefState>(fa.frame()->ref_state())
                          : nullptr;

  const std::vector<Cell> cells = build_cells(fa, filter);
  const auto width = static_cast<std::size_t>(fa.grid()->cells_w());

  out << "{\n";
  write_map_member(out, "blockSize", kBlockSizeMap);
  write_layer(out, "blockSize", cells, width, [](const Cell& c) { return c.block_size; });
  write_map_member(out, "transformSize", kTxSizeMap);
  write_layer(out, "transformSize", cells, width, [](const Cell& c) { return c.tx_size; });
  write_map_member(out, "transformType", kTxTypeMap);
  write_layer(out, "transformType", cells, width, [](const Cell& c) { return c.tx_type; });
  write_map_member(out, "dualFilterType", kDualFilterMap);
  write_layer(out, "dualFilterType", cells, width, [](const Cell& c) { return (c.filter[0] * 3) + c.filter[1]; });
  write_map_member(out, "mode", kPredictionModeMap);
  write_layer(out, "mode", cells, width, [](const Cell& c) { return c.mode; });
  write_map_member(out, "uv_mode", kUvPredictionModeMap);
  write_layer(out, "uv_mode", cells, width, [](const Cell& c) { return c.uv_mode; });
  write_map_member(out, "motion_mode", kMotionModeMap);
  write_layer(out, "motion_mode", cells, width, [](const Cell& c) { return c.motion_mode; });
  write_map_member(out, "compound_type", kCompoundTypeMap);
  write_layer(out, "compound_type", cells, width, [](const Cell& c) { return c.compound_type; });
  write_map_member(out, "skip", kSkipMap);
  write_layer(out, "skip", cells, width, [](const Cell& c) { return c.skip; });
  write_layer(out, "filter", cells, width, [](const Cell& c) { return c.filter; });
  write_layer(out, "cdef_level", cells, width, [](const Cell& c) { return c.cdef_level; });
  write_layer(out, "cdef_strength", cells, width, [](const Cell& c) { return c.cdef_strength; });
  write_layer(out, "cfl_alpha_idx", cells, width, [](const Cell& c) { return c.cfl_alpha_idx; });
  write_layer(out, "cfl_alpha_sign", cells, width, [](const Cell& c) { return c.cfl_alpha_sign; });
  write_layer(out, "delta_q", cells, width, [](const Cell& c) { return c.delta_q; });
  write_layer(out, "seg_id", cells, width, [](const Cell& c) { return c.seg_id; });
  write_layer(out, "motionVectors", cells, width, [](const Cell& c) { return c.mv; });
  write_map_member(out, "intrabc", kIntrabcMap);
  write_layer(out, "intrabc", cells, width, [](const Cell& c) { return c.intrabc; });
  write_map_member(out, "palette", kPaletteMap);
  write_layer(out, "palette", cells, width, [](const Cell& c) { return c.palette; });
  write_map_member(out, "uv_palette", kPaletteMap);
  write_layer(out, "uv_palette", cells, width, [](const Cell& c) { return c.uv_palette; });
  write_map_member(out, "referenceFrame", kRefsMap);
  write_layer(out, "referenceFrame", cells, width, [](const Cell& c) { return c.ref_frame; });
  if (fa.symbols() != nullptr && fa.symbols()->size() != 0) {
    write_accounting(out, fa);
  }

  const auto [tile_cols, tile_rows] = first_tile_mi(fa);
  out << "  \"frame\": " << ordinal << ",\n";
  out << "  \"showFrame\": " << (fa.frame()->shown() ? 1 : 0) << ",\n";
  out << "  \"frameType\": " << (state != nullptr ? static_cast<int>(state->frame_type()) : 0) << ",\n";
  out << "  \"baseQIndex\": " << (filter != nullptr ? static_cast<int>(filter->base_q_idx()) : 0) << ",\n";
  out << "  \"tileCols\": " << tile_cols << ",\n";
  out << "  \"tileRows\": " << tile_rows << ",\n";
  out << "  \"deltaQPresentFlag\": " << (filter != nullptr ? static_cast<int>(filter->delta_q_present()) : 0) << ",\n";
  out << "  \"deltaQRes\": " << (filter != nullptr ? static_cast<int>(filter->delta_q_res()) : 0) << ",\n";
  out << "  \"config\": ";
  write_map(out, kConfigMap);
  out << ",\n";
  out << "  \"configString\": ";
  write_json_string(out, config_string);
  out << "\n},\n";
}

}  // namespace

void write_aom_inspect_json(std::ostream& out, std::span<const fb::FrameAnalysis* const> frames_in_decode_order,
                            const AomJsonOptions& options) {
  const std::string version = options.ovt_version.empty() ? std::string(OVT_VERSION_STRING_VALUE) : options.ovt_version;
  out << "[\n";
  int ordinal = 0;
  for (const fb::FrameAnalysis* fa : frames_in_decode_order) {
    if (fa == nullptr) {
      continue;
    }
    const std::string config_string = "ovt " + version + "; plugin " +
                                      std::string(flatbuffers::GetStringView(fa->plugin_id())) + " " +
                                      std::string(flatbuffers::GetStringView(fa->plugin_version())) +
                                      "; decode_index_offset " + std::to_string(options.decode_index_offset);
    write_frame(out, *fa, ordinal++, config_string);
  }
  out << "null\n]\n";
}

}  // namespace ovt::model
