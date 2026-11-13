// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// YUView statistics CSV exporter and importer (chapter 06 §6.8.2, WI-0.089). Clean-room: written from the
// grammar of §6.8.2 and the "Statistics Extension Syntax" description of appendix F.5, not from YUView code.

#include "ovt/cxx/model/yuview_csv.hpp"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ovt::model {
namespace {

// ---- the fixed type table (§6.8.2) ------------------------------------------------------------------------

enum TypeId : std::uint16_t {
  kPredMode = 0,
  kQp = 1,
  kIntraModeY = 2,
  kIntraModeC = 3,
  kMv0 = 4,
  kMv1 = 5,
  kRef0 = 6,
  kRef1 = 7,
  kTxLog2W = 8,
  kTxLog2H = 9,
  kTxType = 10,
  kCbf = 11,
  kBitsX8 = 12,
  kSkip = 13,
  kLevel = 14,
  kPartitionId = 15,
  kDeblockBsV = 16,
  kDeblockBsH = 17,
  kFilterIdx = 18,
  kPartitionType = 19,
  kMergeOrDirect = 20,
  kMvd0 = 21,
  kMvd1 = 22,
  kBlockGridFirst = 23,  // 23 + BlockKind (MB .. TXB)
  kTypeCount = 32,
};

constexpr std::array<std::string_view, kBlockGridFirst> kTypeNames{
    "OVT_PredMode",      "OVT_QP",         "OVT_IntraModeY", "OVT_IntraModeC", "OVT_MV0",
    "OVT_MV1",           "OVT_Ref0",       "OVT_Ref1",       "OVT_TxLog2W",    "OVT_TxLog2H",
    "OVT_TxType",        "OVT_Cbf",        "OVT_BitsX8",     "OVT_Skip",       "OVT_Level",
    "OVT_PartitionId",   "OVT_DeblockBsV", "OVT_DeblockBsH", "OVT_FilterIdx",  "OVT_PartitionType",
    "OVT_MergeOrDirect", "OVT_MVD0",       "OVT_MVD1",
};

constexpr std::string_view kBlockGridPrefix = "OVT_BlockGrid_";

struct Rgba {
  int r;
  int g;
  int b;
  int a;  // -1 = not written
};

// The 35-entry fixed palette; tools/yuview_palette.json carries the same values (a unit test compares them).
// Entry 0 white, 1 light grey, 2..34 a hue sweep with entry 26 (HEVC vertical) at (0, 128, 255).
constexpr std::array<std::array<int, 3>, 35> kPalette{{
    {255, 255, 255}, {200, 200, 200}, {255, 0, 220}, {255, 0, 174}, {255, 0, 128}, {255, 0, 81},  {255, 0, 35},
    {255, 12, 0},    {255, 58, 0},    {255, 104, 0}, {255, 151, 0}, {255, 197, 0}, {255, 243, 0}, {220, 255, 0},
    {174, 255, 0},   {128, 255, 0},   {81, 255, 0},  {35, 255, 0},  {0, 255, 12},  {0, 255, 58},  {0, 255, 104},
    {0, 255, 151},   {0, 255, 197},   {0, 255, 243}, {0, 220, 255}, {0, 174, 255}, {0, 128, 255}, {0, 81, 255},
    {0, 35, 255},    {12, 0, 255},    {58, 0, 255},  {104, 0, 255}, {151, 0, 255}, {197, 0, 255}, {243, 0, 255},
}};

Rgba palette(std::int64_t value) {
  const auto& c = kPalette[static_cast<std::size_t>(value % static_cast<std::int64_t>(kPalette.size()))];
  return {c[0], c[1], c[2], -1};
}

constexpr std::int64_t kNoValue = std::numeric_limits<std::int64_t>::min();

// ---- small formatting helpers (locale-independent) ---------------------------------------------------------

void append_int(std::string& line, std::int64_t value) {
  std::array<char, 24> buf{};
  const auto res = std::to_chars(buf.data(), std::to_address(buf.end()), value);
  line.append(buf.data(), res.ptr);
}

void append_field(std::string& line, std::int64_t value) {
  line.push_back(';');
  append_int(line, value);
}

// "25", "29.97", "0": fixed notation, at most six decimals, never an exponent (WI-0.089 pitfall).
std::string format_fps(double fps) {
  if (!std::isfinite(fps) || fps <= 0.0 || fps > 1.0e6) {
    return "0";
  }
  const auto micro = static_cast<std::int64_t>(std::llround(fps * 1.0e6));
  std::string out;
  append_int(out, micro / 1'000'000);
  std::int64_t frac = micro % 1'000'000;
  if (frac != 0) {
    std::string digits(6, '0');
    for (std::size_t i = 6; i-- > 0;) {
      digits[i] = static_cast<char>('0' + (frac % 10));
      frac /= 10;
    }
    while (!digits.empty() && digits.back() == '0') {
      digits.pop_back();
    }
    out.push_back('.');
    out += digits;
  }
  return out;
}

std::string sanitize_name(std::string_view name) {
  std::string out(name);
  for (char& c : out) {
    if (c == ';' || c == '\n' || c == '\r') {
      c = '_';
    }
  }
  return out;
}

// ---- reading a FrameAnalysis ---------------------------------------------------------------------------------

struct Row {
  int x;
  int y;
  int w;
  int h;
  std::int64_t value;
  std::int64_t vec_y;  // kNoValue for map/range types
  friend bool operator<(const Row& a, const Row& b) {
    return std::tie(a.y, a.x, a.h, a.w, a.value, a.vec_y) < std::tie(b.y, b.x, b.h, b.w, b.value, b.vec_y);
  }
};

bool is_prediction_kind(fb::BlockKind kind) {
  return kind == fb::BlockKind::MB || kind == fb::BlockKind::CU || kind == fb::BlockKind::Block;
}

// One exported frame with the lookups the row emitters share.
class FrameView {
 public:
  explicit FrameView(const fb::FrameAnalysis& fa) : fa_(&fa) {
    const auto* grid = fa.grid();
    if (grid != nullptr) {
      cells_w_ = grid->cells_w();
      cells_h_ = grid->cells_h();
    }
    const auto* blocks = fa.blocks();
    if (blocks == nullptr) {
      return;
    }
    std::unordered_map<std::uint32_t, const fb::Block*> by_id;
    for (const auto* block : *blocks) {
      if (block != nullptr && block->rect() != nullptr) {
        by_id.emplace(block->id(), block);
      }
    }
    for (const auto* block : *blocks) {
      if (block == nullptr || block->rect() == nullptr || !is_prediction_kind(block->kind())) {
        continue;
      }
      bool leaf = true;
      if (const auto* children = block->children(); children != nullptr) {
        for (const std::uint32_t child : *children) {
          const auto it = by_id.find(child);
          if (it != by_id.end() && is_prediction_kind(it->second->kind())) {
            leaf = false;
            break;
          }
        }
      }
      if (leaf) {
        leaves_.push_back(block);
      }
    }
  }

  [[nodiscard]] const fb::FrameAnalysis& fa() const { return *fa_; }
  [[nodiscard]] const std::vector<const fb::Block*>& leaves() const { return leaves_; }

  // The values of one CellGrid vector as int64 with sentinels mapped to kNoValue; empty when the vector is
  // absent or its length is not cells_w * cells_h.
  [[nodiscard]] std::vector<std::int64_t> cell_values(TypeId type) const {
    const auto* grid = fa_->grid();
    if (grid == nullptr) {
      return {};
    }
    switch (type) {
      case kIntraModeY:
        return copy(grid->intra_mode_y(), std::int16_t{-1});
      case kIntraModeC:
        return copy(grid->intra_mode_c(), std::int16_t{-1});
      case kRef0:
        return copy(grid->ref0(), std::int8_t{-1});
      case kRef1:
        return copy(grid->ref1(), std::int8_t{-1});
      case kTxLog2W:
        return copy(grid->tx_log2_w(), std::uint8_t{255});
      case kTxLog2H:
        return copy(grid->tx_log2_h(), std::uint8_t{255});
      case kTxType:
        return copy(grid->tx_type(), std::uint8_t{255});
      case kCbf:
        return copy(grid->cbf(), std::uint8_t{255});
      case kPartitionId:
        return copy(grid->partition_id(), std::uint16_t{65535});
      case kDeblockBsV:
        return copy(grid->deblock_bs_v(), std::uint8_t{255});
      case kDeblockBsH:
        return copy(grid->deblock_bs_h(), std::uint8_t{255});
      case kFilterIdx:
        return copy(grid->filter_idx(), std::uint8_t{255});
      default:
        return {};
    }
  }

  // Per leaf block if all its cells agree, else per cell; the whole grid per cell when there are no leaves.
  void uniform_rows(const std::vector<std::int64_t>& values, std::vector<Row>& rows) const {
    if (values.empty()) {
      return;
    }
    if (leaves_.empty()) {
      cell_rows(values, rows);
      return;
    }
    for (const auto* block : leaves_) {
      const auto& r = *block->rect();
      const auto [x0, x1] = cell_span(r.x(), r.w(), cells_w_);
      const auto [y0, y1] = cell_span(r.y(), r.h(), cells_h_);
      if (x0 >= x1 || y0 >= y1) {
        continue;
      }
      const std::int64_t first = values[index(x0, y0)];
      const bool uniform = all_equal(values, first, {x0, x1}, {y0, y1});
      if (uniform) {
        if (first != kNoValue) {
          rows.push_back({r.x(), r.y(), r.w(), r.h(), first, kNoValue});
        }
        continue;
      }
      for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
          push_cell(values, x, y, rows);
        }
      }
    }
  }

  void cell_rows(const std::vector<std::int64_t>& values, std::vector<Row>& rows) const {
    if (values.empty()) {
      return;
    }
    for (std::uint32_t y = 0; y < cells_h_; ++y) {
      for (std::uint32_t x = 0; x < cells_w_; ++x) {
        push_cell(values, x, y, rows);
      }
    }
  }

 private:
  using Span = std::pair<std::uint32_t, std::uint32_t>;

  [[nodiscard]] bool all_equal(const std::vector<std::int64_t>& values, std::int64_t first, Span xs, Span ys) const {
    for (std::uint32_t y = ys.first; y < ys.second; ++y) {
      for (std::uint32_t x = xs.first; x < xs.second; ++x) {
        if (values[index(x, y)] != first) {
          return false;
        }
      }
    }
    return true;
  }

  template <typename T>
  [[nodiscard]] std::vector<std::int64_t> copy(const flatbuffers::Vector<T>* vec, T sentinel) const {
    const std::uint64_t cells = std::uint64_t{cells_w_} * cells_h_;
    if (vec == nullptr || vec->size() != cells) {
      return {};
    }
    std::vector<std::int64_t> out;
    out.reserve(vec->size());
    for (const T v : *vec) {
      out.push_back(v == sentinel ? kNoValue : static_cast<std::int64_t>(v));
    }
    return out;
  }

  // Cells [first, last) that luma span [pos, pos + len) touches, clipped to [0, cells).
  static std::pair<std::uint32_t, std::uint32_t> cell_span(int pos, int len, std::uint32_t cells) {
    const std::int64_t lo = std::max<std::int64_t>(0, pos);
    const std::int64_t hi = std::min<std::int64_t>(std::int64_t{pos} + len, std::int64_t{cells} * 4);
    if (lo >= hi) {
      return {0, 0};
    }
    return {static_cast<std::uint32_t>(lo / 4), static_cast<std::uint32_t>((hi + 3) / 4)};
  }

  [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const { return (std::size_t{y} * cells_w_) + x; }

  void push_cell(const std::vector<std::int64_t>& values, std::uint32_t x, std::uint32_t y,
                 std::vector<Row>& rows) const {
    const std::int64_t v = values[index(x, y)];
    if (v != kNoValue) {
      rows.push_back({static_cast<int>(x * 4), static_cast<int>(y * 4), 4, 4, v, kNoValue});
    }
  }

  const fb::FrameAnalysis* fa_;
  std::uint32_t cells_w_ = 0;
  std::uint32_t cells_h_ = 0;
  std::vector<const fb::Block*> leaves_;
};

bool is_block_type(std::uint16_t type) {
  return type == kPredMode || type == kQp || type == kBitsX8 || type == kSkip || type == kLevel ||
         type == kPartitionType || type == kMergeOrDirect;
}

bool is_uniform_type(std::uint16_t type) {
  return type == kIntraModeY || type == kIntraModeC || type == kRef0 || type == kRef1 || type == kFilterIdx ||
         type == kPartitionId;
}

bool is_cell_type(std::uint16_t type) {
  return type == kTxLog2W || type == kTxLog2H || type == kTxType || type == kCbf || type == kDeblockBsV ||
         type == kDeblockBsH;
}

std::int64_t block_value(const fb::Block& block, std::uint16_t type) {
  switch (type) {
    case kPredMode:
      return static_cast<std::int64_t>(block.pred());
    case kQp:
      return block.qp() == -1 ? kNoValue : block.qp();
    case kBitsX8:
      return block.bits_x8();
    case kSkip:
      return block.skip() ? 1 : 0;
    case kLevel:
      return block.level() == 255 ? kNoValue : block.level();
    case kPartitionType:
      return block.partition_type() == 255 ? kNoValue : block.partition_type();
    case kMergeOrDirect:
      return block.merge_or_direct() ? 1 : 0;
    default:
      return kNoValue;
  }
}

void block_rows(const FrameView& view, std::uint16_t type, std::vector<Row>& rows) {
  for (const auto* block : view.leaves()) {
    const std::int64_t v = block_value(*block, type);
    if (v != kNoValue) {
      const auto& r = *block->rect();
      rows.push_back({r.x(), r.y(), r.w(), r.h(), v, kNoValue});
    }
  }
}

// OVT_MV0/1 and OVT_MVD0/1: one row per MotionVector.rect of the list; MVD rows omitted when -32768.
void vector_rows(const fb::FrameAnalysis& fa, std::uint16_t type, std::vector<Row>& rows) {
  const auto* mvs = fa.mvs();
  if (mvs == nullptr) {
    return;
  }
  const bool mvd = type == kMvd0 || type == kMvd1;
  const std::uint8_t list = (type == kMv0 || type == kMvd0) ? 0 : 1;
  for (const auto* mv : *mvs) {
    if (mv == nullptr || mv->rect() == nullptr || mv->list() != list) {
      continue;
    }
    const auto& r = *mv->rect();
    if (!mvd) {
      rows.push_back({r.x(), r.y(), r.w(), r.h(), mv->mv_x(), mv->mv_y()});
    } else if (mv->mvd_x() != -32768 && mv->mvd_y() != -32768) {
      rows.push_back({r.x(), r.y(), r.w(), r.h(), mv->mvd_x(), mv->mvd_y()});
    }
  }
}

// OVT_BlockGrid_<Kind>: value 1 for every block of the kind.
void grid_kind_rows(const fb::FrameAnalysis& fa, std::uint16_t type, std::vector<Row>& rows) {
  const auto* blocks = fa.blocks();
  if (blocks == nullptr) {
    return;
  }
  const auto kind = static_cast<fb::BlockKind>(type - kBlockGridFirst);
  for (const auto* block : *blocks) {
    if (block != nullptr && block->rect() != nullptr && block->kind() == kind) {
      const auto& r = *block->rect();
      rows.push_back({r.x(), r.y(), r.w(), r.h(), 1, kNoValue});
    }
  }
}

// All rows of one type for one frame, sorted by y, x (§6.8.2 ordering).
std::vector<Row> rows_for(const FrameView& view, std::uint16_t type) {
  std::vector<Row> rows;
  if (is_block_type(type)) {
    block_rows(view, type, rows);
  } else if (is_uniform_type(type)) {
    view.uniform_rows(view.cell_values(static_cast<TypeId>(type)), rows);
  } else if (is_cell_type(type)) {
    view.cell_rows(view.cell_values(static_cast<TypeId>(type)), rows);
  } else if (type == kMv0 || type == kMv1 || type == kMvd0 || type == kMvd1) {
    vector_rows(view.fa(), type, rows);
  } else if (type >= kBlockGridFirst && type < kTypeCount) {
    grid_kind_rows(view.fa(), type, rows);
  }
  std::stable_sort(rows.begin(), rows.end());
  return rows;
}

// Whether @p type has a source in @p view (decides whether its type block is written).
bool provides(const FrameView& view, std::uint16_t type) {
  if (is_block_type(type)) {
    return !view.leaves().empty();
  }
  if (is_uniform_type(type) || is_cell_type(type)) {
    return !view.cell_values(static_cast<TypeId>(type)).empty();
  }
  return !rows_for(view, type).empty();
}

std::string type_name(std::uint16_t type) {
  if (type < kBlockGridFirst) {
    return std::string(kTypeNames[type]);
  }
  return std::string(kBlockGridPrefix) + fb::EnumNameBlockKind(static_cast<fb::BlockKind>(type - kBlockGridFirst));
}

std::string_view type_kind(std::uint16_t type) {
  switch (type) {
    case kQp:
    case kTxLog2W:
    case kTxLog2H:
    case kBitsX8:
    case kLevel:
    case kPartitionId:
    case kDeblockBsV:
    case kDeblockBsH:
      return "range";
    case kMv0:
    case kMv1:
    case kMvd0:
    case kMvd1:
      return "vector";
    default:
      return "map";
  }
}

// ---- header writer -----------------------------------------------------------------------------------------

struct FileStats {
  std::int64_t max_bits = 0;
  std::int64_t partitions = 1;
  std::set<std::int64_t> intra_y;
  std::set<std::int64_t> intra_c;
  std::set<std::int64_t> partition_types;
};

void write_line(std::ostream& out, const std::string& line) {
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
  out.put('\n');
}

void map_color(std::ostream& out, std::int64_t value, Rgba c) {
  std::string line = "%;mapColor";
  append_field(line, value);
  append_field(line, c.r);
  append_field(line, c.g);
  append_field(line, c.b);
  if (c.a >= 0) {
    append_field(line, c.a);
  }
  write_line(out, line);
}

void default_range(std::ostream& out, std::int64_t lo, std::int64_t hi, std::string_view colormap) {
  std::string line = "%;defaultRange";
  append_field(line, lo);
  append_field(line, hi);
  line.push_back(';');
  line += colormap;
  write_line(out, line);
}

void vector_attrs(std::ostream& out, Rgba c, bool scale_to_block) {
  std::string line = "%;vectorColor";
  append_field(line, c.r);
  append_field(line, c.g);
  append_field(line, c.b);
  append_field(line, c.a);
  write_line(out, line);
  write_line(out, "%;scaleFactor;16");
  if (scale_to_block) {
    write_line(out, "%;scaleToBlockSize;0");
  }
}

void palette_colors(std::ostream& out, const std::set<std::int64_t>& values) {
  for (const std::int64_t v : values) {
    if (v >= 0) {
      map_color(out, v, palette(v));
    }
  }
}

void palette_range(std::ostream& out, std::int64_t last) {
  for (std::int64_t v = 0; v <= last; ++v) {
    map_color(out, v, palette(v));
  }
}

void write_type_attrs(std::ostream& out, std::uint16_t type, fb::Codec codec, const FileStats& stats) {
  switch (type) {
    case kPredMode: {
      constexpr std::array<Rgba, 7> kPred{{{128, 128, 128, -1},
                                           {255, 0, 0, -1},
                                           {0, 0, 255, -1},
                                           {0, 255, 0, -1},
                                           {255, 0, 255, -1},
                                           {255, 255, 0, -1},
                                           {0, 255, 255, -1}}};
      for (std::size_t v = 0; v < kPred.size(); ++v) {
        map_color(out, static_cast<std::int64_t>(v), kPred[v]);
      }
      break;
    }
    case kQp:
      default_range(out, 0, codec == fb::Codec::AV1 ? 255 : 51, "jet");
      break;
    case kIntraModeY:
      palette_colors(out, stats.intra_y);
      break;
    case kIntraModeC:
      palette_colors(out, stats.intra_c);
      break;
    case kMv0:
      vector_attrs(out, {0, 255, 0, 255}, true);
      break;
    case kMv1:
      vector_attrs(out, {255, 0, 255, 255}, true);
      break;
    case kRef0:
    case kRef1:
      map_color(out, -1, {0, 0, 0, -1});
      palette_range(out, 15);
      break;
    case kTxLog2W:
    case kTxLog2H:
      default_range(out, 2, 6, "heat");
      break;
    case kTxType:
      palette_range(out, 16);
      break;
    case kCbf:
      for (int v = 0; v < 8; ++v) {
        map_color(out, v, {(v & 1) != 0 ? 255 : 0, (v & 2) != 0 ? 255 : 0, (v & 4) != 0 ? 255 : 0, -1});
      }
      break;
    case kBitsX8:
      default_range(out, 0, stats.max_bits, "hot");
      break;
    case kSkip:
      map_color(out, 0, {0, 0, 0, 0});
      map_color(out, 1, {0, 255, 0, 128});
      break;
    case kLevel:
      default_range(out, 1, 6, "cool");
      break;
    case kPartitionId:
      default_range(out, 0, stats.partitions - 1, "lines");
      break;
    case kDeblockBsV:
    case kDeblockBsH:
      default_range(out, 0, 4, "gray");
      break;
    case kFilterIdx:
      palette_range(out, 7);
      break;
    case kPartitionType:
      switch (codec) {
        case fb::Codec::H264:
          palette_range(out, 56);
          break;
        case fb::Codec::H265:
          palette_range(out, 7);
          break;
        case fb::Codec::AV1:
          palette_range(out, 9);
          break;
        default:
          palette_colors(out, stats.partition_types);
          break;
      }
      break;
    case kMergeOrDirect:
      map_color(out, 0, {0, 0, 0, 0});
      map_color(out, 1, {255, 128, 0, 160});
      break;
    case kMvd0:
      vector_attrs(out, {0, 128, 0, 255}, false);
      break;
    case kMvd1:
      vector_attrs(out, {128, 0, 128, 255}, false);
      break;
    default:  // OVT_BlockGrid_<Kind>
      map_color(out, 1, {255, 255, 255, 0});
      break;
  }
}

bool is_exported(const fb::FrameAnalysis* fa) {
  if (fa == nullptr || fa->frame() == nullptr || !fa->frame()->shown()) {
    return false;
  }
  const auto structure = fa->frame()->structure();
  return structure != fb::PictureStructure::TopField && structure != fb::PictureStructure::BottomField;
}

using Frames = std::vector<std::pair<std::int64_t, FrameView>>;  // (POC column value, frame)
using TypeSet = std::array<bool, kTypeCount>;

// Exported frames grouped by POC column value, ascending; equal values keep display order.
Frames collect_frames(std::span<const fb::FrameAnalysis* const> frames_in_display_order, bool poc_column_is_poc) {
  Frames frames;
  for (const auto* fa : frames_in_display_order) {
    if (is_exported(fa)) {
      const std::int64_t poc = poc_column_is_poc ? fa->frame()->poc() : fa->frame()->id()->display_index();
      frames.emplace_back(poc, FrameView(*fa));
    }
  }
  std::stable_sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return frames;
}

TypeSet selected_types(const std::vector<std::uint16_t>& types) {
  TypeSet selected{};
  if (types.empty()) {
    selected.fill(true);
  }
  for (const std::uint16_t t : types) {
    if (t < kTypeCount) {
      selected[t] = true;
    }
  }
  return selected;
}

void add_stats(const FrameView& view, FileStats& stats) {
  for (const auto* block : view.leaves()) {
    stats.max_bits = std::max<std::int64_t>(stats.max_bits, block->bits_x8());
    if (block->partition_type() != 255) {
      stats.partition_types.insert(block->partition_type());
    }
  }
  if (const auto* parts = view.fa().partitions(); parts != nullptr) {
    stats.partitions = std::max<std::int64_t>(stats.partitions, parts->size());
  }
  for (const auto& row : rows_for(view, kIntraModeY)) {
    stats.intra_y.insert(row.value);
  }
  for (const auto& row : rows_for(view, kIntraModeC)) {
    stats.intra_c.insert(row.value);
  }
}

void write_header(std::ostream& out, const Frames& frames, const TypeSet& present, fb::Codec codec,
                  const FileStats& stats, const YuviewOptions& options) {
  write_line(out, "%;syntax-version;v1.2");
  std::string seq = "%;seq-specs;" + sanitize_name(options.basename) + ";0";
  const fb::Rect* coded = frames.empty() ? nullptr : frames.front().second.fa().frame()->coded_size();
  append_field(seq, coded != nullptr ? coded->w() : 0);
  append_field(seq, coded != nullptr ? coded->h() : 0);
  seq += ";" + format_fps(options.fps);
  write_line(out, seq);
  for (std::uint16_t t = 0; t < kTypeCount; ++t) {
    if (present[t]) {
      std::string line = "%;type";
      append_field(line, t);
      line += ";" + type_name(t) + ";";
      line += type_kind(t);
      write_line(out, line);
      write_type_attrs(out, t, codec, stats);
    }
  }
}

void write_rows(std::ostream& out, std::int64_t poc, std::uint16_t type, const std::vector<Row>& rows) {
  for (const auto& row : rows) {
    std::string line;
    append_int(line, poc);
    append_field(line, row.x);
    append_field(line, row.y);
    append_field(line, row.w);
    append_field(line, row.h);
    append_field(line, type);
    append_field(line, row.value);
    if (row.vec_y != kNoValue) {
      append_field(line, row.vec_y);
    }
    write_line(out, line);
  }
}

}  // namespace

void write_yuview_csv(std::ostream& out, const fb::StreamIndex& index,
                      std::span<const fb::FrameAnalysis* const> frames_in_display_order, const YuviewOptions& options) {
  const Frames frames = collect_frames(frames_in_display_order, options.poc_column_is_poc);
  const TypeSet selected = selected_types(options.types);
  TypeSet present{};
  FileStats stats;
  for (const auto& entry : frames) {
    for (std::uint16_t t = 0; t < kTypeCount; ++t) {
      present[t] = present[t] || (selected[t] && provides(entry.second, t));
    }
    add_stats(entry.second, stats);
  }
  write_header(out, frames, present, index.codec(), stats, options);

  // Data rows: per POC value, per type, the rows of every frame with that value sorted by y, x.
  for (std::size_t i = 0; i < frames.size();) {
    std::size_t end = i;
    while (end < frames.size() && frames[end].first == frames[i].first) {
      ++end;
    }
    for (std::uint16_t t = 0; t < kTypeCount; ++t) {
      if (!present[t]) {
        continue;
      }
      std::vector<Row> rows;
      for (std::size_t f = i; f < end; ++f) {
        const auto part = rows_for(frames[f].second, t);
        rows.insert(rows.end(), part.begin(), part.end());
      }
      std::stable_sort(rows.begin(), rows.end());
      write_rows(out, frames[i].first, t, rows);
    }
    i = end;
  }
}

// ---- importer ------------------------------------------------------------------------------------------------

namespace {

// SPEC-ASSUMPTION(#233): the importer's diagnostic codes MDL-CSV-0001..0004 are not yet in the §6.9.1
// registry, and it refuses seq-specs sizes above 16384 per side or 8192 x 4320 luma samples in total.
constexpr std::int64_t kMaxSide = 16384;
constexpr std::int64_t kMaxArea = std::int64_t{8192} * 4320;
constexpr std::size_t kMaxNameDiagnostics = 64;

std::vector<std::string_view> split(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = line.find(';', start);
    if (pos == std::string_view::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, pos - start));
    start = pos + 1;
  }
}

std::optional<std::int64_t> parse_int(std::string_view text) {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  std::int64_t value = 0;
  const auto* end = text.data() + text.size();
  const auto res = std::from_chars(text.data(), end, value);
  if (text.empty() || res.ec != std::errc{} || res.ptr != end) {
    return std::nullopt;
  }
  return value;
}

// One CellGrid field the importer fills: value range and sentinel, plus where it lands in CellGridT.
struct GridTarget {
  std::int64_t min;
  std::int64_t max;
  std::int64_t fill;  // sentinel (or 0 for motion vectors)
};

std::optional<GridTarget> grid_target(std::string_view name) {
  constexpr GridTarget kU8{0, 254, 255};
  constexpr GridTarget kS16{-32768, 32767, -1};
  if (name == "OVT_PredMode" || name == "OVT_TxLog2W" || name == "OVT_TxLog2H" || name == "OVT_TxType" ||
      name == "OVT_Cbf" || name == "OVT_DeblockBsV" || name == "OVT_DeblockBsH" || name == "OVT_FilterIdx") {
    return kU8;
  }
  if (name == "OVT_QP" || name == "OVT_IntraModeY" || name == "OVT_IntraModeC") {
    return kS16;
  }
  if (name == "OVT_Ref0" || name == "OVT_Ref1") {
    return GridTarget{-128, 127, -1};
  }
  if (name == "OVT_PartitionId") {
    return GridTarget{0, 65534, 65535};
  }
  if (name == "OVT_MV0" || name == "OVT_MV1") {
    return GridTarget{-32768, 32767, 0};
  }
  return std::nullopt;
}

bool is_ovt_name(std::string_view name) {
  if (name.starts_with(kBlockGridPrefix)) {
    return true;
  }
  return std::find(kTypeNames.begin(), kTypeNames.end(), name) != kTypeNames.end();
}

template <typename T>
std::vector<T> narrow(const std::vector<std::int32_t>& in) {
  std::vector<T> out;
  out.reserve(in.size());
  for (const std::int32_t v : in) {
    out.push_back(static_cast<T>(v));
  }
  return out;
}

class Importer {
 public:
  explicit Importer(int poc) : poc_(poc) {}

  void line(std::string_view text, std::uint64_t number) {
    if (!text.empty() && text.back() == '\r') {
      text.remove_suffix(1);
    }
    if (text.empty()) {
      return;
    }
    const auto fields = split(text);
    if (text.front() == '%') {
      header(fields, number);
    } else {
      row(fields, number);
    }
  }

  // Every declared type with a grid field gets its vector, even when no row of this POC covers it.
  void declare_vectors() {
    for (const auto& entry : types_) {
      const std::string& name = entry.second;
      const auto target = grid_target(name);
      if (!target) {
        continue;
      }
      const bool is_vector = name == "OVT_MV0" || name == "OVT_MV1";
      for (const std::string& key : {name, name + ".y"}) {
        auto& values = grids_[key];
        if (values.empty()) {
          values.assign(std::size_t{cells_w_} * cells_h_, static_cast<std::int32_t>(target->fill));
        }
        if (!is_vector) {
          break;
        }
      }
    }
  }

  std::vector<std::uint8_t> finish() {
    fb::FrameAnalysisT fa;
    fa.schema_version = 1;
    fa.plugin_id = "yuview-csv-import";
    fa.plugin_version = "1";
    if (cells_w_ == 0) {
      diagnostic(fb::Severity::Error, "MDL-CSV-0004",
                 "no usable seq-specs line (missing, malformed or larger than the importer accepts); no grid");
    } else {
      auto grid = std::make_unique<fb::CellGridT>();
      grid->cells_w = cells_w_;
      grid->cells_h = cells_h_;
      declare_vectors();
      for (auto& [name, values] : grids_) {
        assign(*grid, name, values);
      }
      fa.grid = std::move(grid);
    }
    if (malformed_ != 0) {
      std::string msg = std::to_string(malformed_) + " malformed, out-of-range or undeclared line(s) skipped";
      msg += " (first at line " + std::to_string(first_malformed_) + ")";
      diagnostic(fb::Severity::Warning, "MDL-CSV-0001", msg);
    }
    for (auto& d : diagnostics_) {
      fa.diagnostics.push_back(std::move(d));
    }
    flatbuffers::FlatBufferBuilder builder;
    fb::FinishFrameAnalysisBuffer(builder, fb::FrameAnalysis::Pack(builder, &fa));
    const std::span<const std::uint8_t> bytes(builder.GetBufferPointer(), builder.GetSize());
    return {bytes.begin(), bytes.end()};
  }

 private:
  void header(const std::vector<std::string_view>& f, std::uint64_t number) {
    if (f.size() < 2 || f[0] != "%") {
      return;  // comment or unknown header form: ignored
    }
    if (f[1] == "seq-specs") {
      if (seq_seen_) {
        return;
      }
      seq_seen_ = true;
      const auto w = f.size() > 4 ? parse_int(f[4]) : std::nullopt;
      const auto h = f.size() > 5 ? parse_int(f[5]) : std::nullopt;
      if (!w || !h || *w <= 0 || *h <= 0 || *w > kMaxSide || *h > kMaxSide || *w * *h > kMaxArea) {
        bad(number);
        return;
      }
      cells_w_ = static_cast<std::uint32_t>((*w + 3) / 4);
      cells_h_ = static_cast<std::uint32_t>((*h + 3) / 4);
      budget_ = std::uint64_t{cells_w_} * cells_h_ * 32;
    } else if (f[1] == "type") {
      const auto id = f.size() > 3 ? parse_int(f[2]) : std::nullopt;
      if (!id || *id < 0 || *id > 65535 || f[3].empty()) {
        bad(number);
        return;
      }
      types_[*id] = std::string(f[3]);
    }
  }

  void row(const std::vector<std::string_view>& f, std::uint64_t number) {
    if (f.size() < 7 || f.size() > 8) {
      bad(number);
      return;
    }
    std::array<std::int64_t, 8> v{};
    for (std::size_t i = 0; i < f.size(); ++i) {
      const auto parsed = parse_int(f[i]);
      if (!parsed) {
        bad(number);
        return;
      }
      v[i] = *parsed;
    }
    const auto type = types_.find(v[5]);
    if (type == types_.end() || cells_w_ == 0) {
      bad(number);
      return;
    }
    if (v[0] != poc_) {
      return;
    }
    const std::string& name = type->second;
    const auto target = grid_target(name);
    if (!target) {
      note_name(name);
      return;
    }
    const bool is_vector = name == "OVT_MV0" || name == "OVT_MV1";
    constexpr std::int64_t kMaxCoord = std::int64_t{1} << 31;
    if (v[3] <= 0 || v[4] <= 0 || v[3] > kMaxCoord || v[4] > kMaxCoord || v[1] < -kMaxCoord || v[1] > kMaxCoord ||
        v[2] < -kMaxCoord || v[2] > kMaxCoord || v[6] < target->min || v[6] > target->max ||
        (is_vector != (f.size() == 8)) || (is_vector && (v[7] < target->min || v[7] > target->max))) {
      bad(number);
      return;
    }
    paint(name, *target, v[1], v[2], v[3], v[4], v[6]);
    if (is_vector) {
      paint(name + ".y", *target, v[1], v[2], v[3], v[4], v[7]);
    }
  }

  void paint(const std::string& key, const GridTarget& target, std::int64_t x, std::int64_t y, std::int64_t w,
             std::int64_t h, std::int64_t value) {
    // row() bounds x, y, w, h to ±2^31, so none of this overflows.
    const std::int64_t width = std::int64_t{cells_w_} * 4;
    const std::int64_t height = std::int64_t{cells_h_} * 4;
    const std::int64_t x0 = std::clamp<std::int64_t>(x, 0, width);
    const std::int64_t y0 = std::clamp<std::int64_t>(y, 0, height);
    const std::int64_t x1 = std::clamp<std::int64_t>(x + w, 0, width);
    const std::int64_t y1 = std::clamp<std::int64_t>(y + h, 0, height);
    auto& values = grids_[key];
    if (values.empty()) {
      values.assign(std::size_t{cells_w_} * cells_h_, static_cast<std::int32_t>(target.fill));
    }
    for (std::int64_t cy = y0 / 4; cy < (y1 + 3) / 4; ++cy) {
      for (std::int64_t cx = x0 / 4; cx < (x1 + 3) / 4; ++cx) {
        if (budget_ == 0) {
          return;  // pathological input: stop painting, the grid stays partial
        }
        --budget_;
        values[(static_cast<std::size_t>(cy) * cells_w_) + static_cast<std::size_t>(cx)] =
            static_cast<std::int32_t>(value);
      }
    }
  }

  static void assign(fb::CellGridT& grid, const std::string& name, const std::vector<std::int32_t>& v) {
    if (name == "OVT_PredMode") {
      grid.pred = narrow<std::uint8_t>(v);
    } else if (name == "OVT_QP") {
      grid.qp = narrow<std::int16_t>(v);
    } else if (name == "OVT_IntraModeY") {
      grid.intra_mode_y = narrow<std::int16_t>(v);
    } else if (name == "OVT_IntraModeC") {
      grid.intra_mode_c = narrow<std::int16_t>(v);
    } else if (name == "OVT_MV0") {
      grid.mv0_x = narrow<std::int16_t>(v);
    } else if (name == "OVT_MV0.y") {
      grid.mv0_y = narrow<std::int16_t>(v);
    } else if (name == "OVT_MV1") {
      grid.mv1_x = narrow<std::int16_t>(v);
    } else if (name == "OVT_MV1.y") {
      grid.mv1_y = narrow<std::int16_t>(v);
    } else if (name == "OVT_Ref0") {
      grid.ref0 = narrow<std::int8_t>(v);
    } else if (name == "OVT_Ref1") {
      grid.ref1 = narrow<std::int8_t>(v);
    } else if (name == "OVT_TxLog2W") {
      grid.tx_log2_w = narrow<std::uint8_t>(v);
    } else if (name == "OVT_TxLog2H") {
      grid.tx_log2_h = narrow<std::uint8_t>(v);
    } else if (name == "OVT_TxType") {
      grid.tx_type = narrow<std::uint8_t>(v);
    } else if (name == "OVT_Cbf") {
      grid.cbf = narrow<std::uint8_t>(v);
    } else if (name == "OVT_PartitionId") {
      grid.partition_id = narrow<std::uint16_t>(v);
    } else if (name == "OVT_DeblockBsV") {
      grid.deblock_bs_v = narrow<std::uint8_t>(v);
    } else if (name == "OVT_DeblockBsH") {
      grid.deblock_bs_h = narrow<std::uint8_t>(v);
    } else if (name == "OVT_FilterIdx") {
      grid.filter_idx = narrow<std::uint8_t>(v);
    }
  }

  void note_name(const std::string& name) {
    if (noted_.size() >= kMaxNameDiagnostics || !noted_.insert(name).second) {
      return;
    }
    if (is_ovt_name(name)) {
      diagnostic(fb::Severity::Info, "MDL-CSV-0003", "OVT type '" + name + "' has no CellGrid field; not imported");
    } else {
      diagnostic(fb::Severity::Info, "MDL-CSV-0002", "type '" + name + "' is not an OVT type; not imported");
    }
  }

  void diagnostic(fb::Severity severity, std::string code, std::string message) {
    auto d = std::make_unique<fb::DiagnosticT>();
    d->severity = severity;
    d->code = std::move(code);
    d->message = std::move(message);
    d->spec_ref = "06 §6.8.2";
    diagnostics_.push_back(std::move(d));
  }

  void bad(std::uint64_t number) {
    if (malformed_++ == 0) {
      first_malformed_ = number;
    }
  }

  int poc_;
  bool seq_seen_ = false;
  std::uint32_t cells_w_ = 0;
  std::uint32_t cells_h_ = 0;
  std::uint64_t budget_ = 0;
  std::map<std::int64_t, std::string> types_;
  std::map<std::string, std::vector<std::int32_t>> grids_;
  std::set<std::string> noted_;
  std::vector<std::unique_ptr<fb::DiagnosticT>> diagnostics_;
  std::uint64_t malformed_ = 0;
  std::uint64_t first_malformed_ = 0;
};

}  // namespace

std::vector<std::uint8_t> read_yuview_csv(std::istream& in, int poc) {
  Importer importer(poc);
  std::string text;
  std::uint64_t number = 0;
  while (std::getline(in, text)) {
    importer.line(text, ++number);
  }
  return importer.finish();
}

}  // namespace ovt::model
