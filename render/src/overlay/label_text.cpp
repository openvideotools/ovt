// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Block label strings (chapter 10 §10.5.5, WI-2.433 step 2). Values come from the Block table where it has them (size,
// prediction, QP, bits, id), from FrameAnalysis.mvs for MvValues and from the CellGrid cell at the block's top-left
// corner for the per-cell columns (intra mode, transform type, reference index), whose §6.3.1 numbering they use.

#include "overlay/label_text.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ovt::render::detail {
namespace {

constexpr std::string_view kMinus = "−";  // U+2212, one of the atlas's extra glyphs (§10.5.1)

// clang-format off
constexpr std::array<std::string_view, 9> kH264Intra4x4 = {"V", "H", "DC", "DDL", "DDR", "VR", "HD", "VL", "HU"};
constexpr std::array<std::string_view, 4> kH264Intra16x16 = {"V", "H", "DC", "PLANE"};
constexpr std::array<std::string_view, 13> kAv1YMode = {
    "DC_PRED", "V_PRED", "H_PRED", "D45_PRED", "D135_PRED", "D113_PRED", "D157_PRED", "D203_PRED", "D67_PRED",
    "SMOOTH_PRED", "SMOOTH_V_PRED", "SMOOTH_H_PRED", "PAETH_PRED"};
constexpr std::array<std::string_view, 4> kH264TxType = {"DCT4", "DCT8", "DCT4+DC", "PCM"};
constexpr std::array<std::string_view, 4> kH265TxType = {"DCT2", "DST7", "TSKIP", "BYPASS"};
constexpr std::array<std::string_view, 17> kAv1TxType = {
    "DCT_DCT", "ADST_DCT", "DCT_ADST", "ADST_ADST", "FLIPADST_DCT", "DCT_FLIPADST", "FLIPADST_FLIPADST",
    "ADST_FLIPADST", "FLIPADST_ADST", "IDTX", "V_DCT", "H_DCT", "V_ADST", "H_ADST", "V_FLIPADST", "H_FLIPADST", "WHT"};
// clang-format on

template <size_t N>
std::string named(const std::array<std::string_view, N>& table, int value) {
  if (value >= 0 && static_cast<size_t>(value) < N) {
    return std::string(table.at(static_cast<size_t>(value)));
  }
  return std::to_string(value);
}

// "+3", "−12", "0".
std::string signed_text(int v) {
  if (v == 0) {
    return "0";
  }
  return (v < 0 ? std::string(kMinus) : std::string("+")) + std::to_string(v < 0 ? -v : v);
}

// Index of the CellGrid cell under the block's top-left corner, or -1.
int64_t top_left_cell(const ovt::fb::Block& b, const ovt::fb::CellGrid* grid) {
  if (grid == nullptr || b.rect() == nullptr || b.rect()->x() < 0 || b.rect()->y() < 0) {
    return -1;
  }
  const auto cx = static_cast<uint32_t>(b.rect()->x()) / 4;
  const auto cy = static_cast<uint32_t>(b.rect()->y()) / 4;
  if (cx >= grid->cells_w() || cy >= grid->cells_h()) {
    return -1;
  }
  return (static_cast<int64_t>(cy) * grid->cells_w()) + cx;
}

template <typename V>
bool has_cell(const V* v, int64_t cell) {
  return v != nullptr && cell >= 0 && static_cast<uint64_t>(cell) < v->size();
}

std::string pred_text(const ovt::fb::Block& b, const ovt::fb::CellGrid* grid, int64_t cell) {
  if (b.skip()) {
    return "S";
  }
  switch (b.pred()) {
    case ovt::fb::PredKind::Intra:
      return "I";
    case ovt::fb::PredKind::Skip:
      return "S";
    case ovt::fb::PredKind::Inter: {
      const bool l1 = grid != nullptr && has_cell(grid->ref1(), cell) &&
                      grid->ref1()->Get(static_cast<flatbuffers::uoffset_t>(cell)) >= 0;
      return l1 ? "B" : "P";
    }
    case ovt::fb::PredKind::IntraBC:
      return "IBC";
    case ovt::fb::PredKind::PCM:
      return "PCM";
    case ovt::fb::PredKind::Palette:
      return "PLT";
    default:
      return "";
  }
}

// a / b rounded towards minus infinity, as the codecs' arithmetic >> of a negative MV (b > 0).
int floor_div(int a, int b) {
  return (a / b) - ((a % b) != 0 && a < 0 ? 1 : 0);
}

// "L0 −12,+3" per list, in quarter-sample units (H.26x) or eighth-sample units (AV1); the first MV of each list.
std::string mv_text(const ovt::fb::FrameAnalysis& fa, uint32_t block_id, LabelCodec codec) {
  if (fa.mvs() == nullptr) {
    return "";
  }
  const int shift = codec == LabelCodec::Av1 ? 1 : 2;  // storage is 1/16 sample
  std::array<std::string, 2> lists;
  for (const ovt::fb::MotionVector* mv : *fa.mvs()) {
    if (mv->block_id() != block_id || mv->list() > 1 || !lists.at(mv->list()).empty()) {
      continue;
    }
    const int x = floor_div(mv->mv_x(), 1 << shift);
    const int y = floor_div(mv->mv_y(), 1 << shift);
    lists.at(mv->list()) = "L" + std::to_string(mv->list()) + " " + signed_text(x) + "," + signed_text(y);
  }
  if (lists[0].empty() || lists[1].empty()) {
    return lists[0] + lists[1];
  }
  return lists[0] + "\n" + lists[1];
}

// "L0[0]" per list from ref0 / ref1 of the cell (RefSlot.name when the plugin gave one).
std::string ref_text(const ovt::fb::FrameAnalysis& fa, int64_t cell) {
  const ovt::fb::CellGrid* grid = fa.grid();
  std::string out;
  for (int list = 0; list < 2; ++list) {
    const auto* col = list == 0 ? grid->ref0() : grid->ref1();
    if (!has_cell(col, cell)) {
      continue;
    }
    const int8_t raw = col->Get(static_cast<flatbuffers::uoffset_t>(cell));
    if (raw < 0) {
      continue;
    }
    const auto ref = static_cast<int>(static_cast<uint8_t>(raw));
    // CellGrid.ref0/ref1 index FrameAnalysis.refs, whose names ("L0[0]", "LAST") the plugin chose.
    std::string name = "L" + std::to_string(list) + "[" + std::to_string(ref) + "]";
    if (fa.refs() != nullptr && static_cast<flatbuffers::uoffset_t>(ref) < fa.refs()->size()) {
      const ovt::fb::RefSlot* slot = fa.refs()->Get(static_cast<flatbuffers::uoffset_t>(ref));
      if (slot->name() != nullptr && !slot->name()->empty()) {
        name = slot->name()->str();
      }
    }
    out += (out.empty() ? "" : "\n") + name;
  }
  return out;
}

std::string label_text(const ovt::fb::FrameAnalysis& fa, const ovt::fb::Block& b, LabelContent content,
                       LabelCodec codec) {
  const ovt::fb::CellGrid* grid = fa.grid();
  const int64_t cell = top_left_cell(b, grid);
  switch (content) {
    case LabelContent::BlockSize:
      return std::to_string(b.rect()->w()) + "x" + std::to_string(b.rect()->h());
    case LabelContent::PredMode:
      return pred_text(b, grid, cell);
    case LabelContent::QP:
      return std::to_string(b.qp());
    case LabelContent::Bits:
      return std::to_string(b.bits_x8() / 8) + "b";
    case LabelContent::MvValues:
      return mv_text(fa, b.id(), codec);
    case LabelContent::IntraMode:
      if (grid != nullptr && has_cell(grid->intra_mode_y(), cell)) {
        const int m = grid->intra_mode_y()->Get(static_cast<flatbuffers::uoffset_t>(cell));
        return m < 0 ? "" : intra_mode_label(codec, m);
      }
      return "";
    case LabelContent::TxType:
      if (grid != nullptr && has_cell(grid->tx_type(), cell)) {
        const int t = grid->tx_type()->Get(static_cast<flatbuffers::uoffset_t>(cell));
        return t == 0xFF ? "" : tx_type_label(codec, t);
      }
      return "";
    case LabelContent::RefIdx:
      return grid != nullptr ? ref_text(fa, cell) : "";
    case LabelContent::BlockId:
      return std::to_string(b.id());
    case LabelContent::None:
      break;
  }
  return "";
}

}  // namespace

std::string intra_mode_label(LabelCodec codec, int value) {
  switch (codec) {
    case LabelCodec::H264:
      if (value >= 16 && value <= 19) {
        return "I16 " + named(kH264Intra16x16, value - 16);
      }
      return value == 32 ? "PCM" : named(kH264Intra4x4, value);
    case LabelCodec::H265:
      if (value == 0) {
        return "PLANAR";
      }
      if (value == 1) {
        return "DC";
      }
      if (value >= 2 && value <= 34) {
        return "ANG " + std::to_string(value);
      }
      return value == 64 ? "PCM" : value == 65 ? "PLT" : std::to_string(value);
    case LabelCodec::Av1:
      return value == 32 ? "IBC" : value == 33 ? "PLT" : named(kAv1YMode, value);
    case LabelCodec::Unknown:
      break;
  }
  return std::to_string(value);
}

std::string tx_type_label(LabelCodec codec, int value) {
  switch (codec) {
    case LabelCodec::H264:
      return named(kH264TxType, value);
    case LabelCodec::H265:
      return named(kH265TxType, value);
    case LabelCodec::Av1:
      return named(kAv1TxType, value);
    case LabelCodec::Unknown:
      break;
  }
  return std::to_string(value);
}

void build_block_labels(const ovt::fb::FrameAnalysis& fa, LabelContent content, LabelCodec codec, OverlaySet& set) {
  if (content == LabelContent::None || fa.blocks() == nullptr) {
    return;
  }
  for (const ovt::fb::Block* b : *fa.blocks()) {
    if ((b->children() != nullptr && !b->children()->empty()) || b->rect() == nullptr || b->rect()->w() <= 0 ||
        b->rect()->h() <= 0) {
      continue;
    }
    std::string text = label_text(fa, *b, content, codec);
    if (text.empty()) {
      continue;
    }
    const ovt::fb::Rect& r = *b->rect();
    BlockLabel label;
    label.cx = static_cast<float>(r.x()) + (static_cast<float>(r.w()) / 2.0F);
    label.cy = static_cast<float>(r.y()) + (static_cast<float>(r.h()) / 2.0F);
    label.min_side = static_cast<float>(r.w() < r.h() ? r.w() : r.h());
    label.text = std::move(text);
    set.block_labels.push_back(std::move(label));
  }
}

}  // namespace ovt::render::detail
