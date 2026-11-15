// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The CellGrid half of the chapter 07 §7.6.2 mapping (WI-2.164): per-cell vectors read straight from libde265's
// per-4x4 metadata, the reference slot table, and serialisation. The block-tree mapping (the next part of WI-2.164)
// adds the fields that need the tree (block_id and the transform fields). Internal to the plugin.
#ifndef OVT_PLUGIN_DE265_MAP_GRID_HPP
#define OVT_PLUGIN_DE265_MAP_GRID_HPP

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "map_blocks.hpp"
#include "ovt_model_generated.h"

namespace ovt::de265 {

constexpr uint8_t kNotApplicable = 0xFF;

// ovt.fb.PredKind values.
constexpr uint8_t kPredNone = 0;
constexpr uint8_t kPredIntra = 1;
constexpr uint8_t kPredInter = 2;
constexpr uint8_t kPredSkip = 3;
constexpr uint8_t kPredPcm = 5;

/// CellGrid.intra_mode_y for a PCM CU (chapter 06 §6.3.1).
constexpr int16_t kIntraModePcm = 64;

/// Quarter-sample vector -> 1/16 luma sample units (chapter 0 §0.1.9), saturated to int16: H.265 allows
/// -2^15..2^15-1 quarter samples.
inline int16_t mv16(int16_t quarter) {
  return static_cast<int16_t>(std::clamp<int32_t>(int32_t{quarter} * 4, std::numeric_limits<int16_t>::min(),
                                                  std::numeric_limits<int16_t>::max()));
}

/// The CellGrid vectors before serialisation, one value per cell.
struct GridCells {
  explicit GridCells(std::size_t n);
  std::vector<uint32_t> block_id;
  std::vector<uint16_t> partition_id;
  std::vector<uint8_t> pred;
  std::vector<int16_t> qp;
  std::vector<int16_t> intra_mode_y;
  std::vector<int16_t> intra_mode_c;
  std::vector<int16_t> mv0_x;
  std::vector<int16_t> mv0_y;
  std::vector<int16_t> mv1_x;
  std::vector<int16_t> mv1_y;
  std::vector<int8_t> ref0;
  std::vector<int8_t> ref1;
  std::vector<uint8_t> tx_log2;  // tx_log2_w = tx_log2_h (HEVC transforms are square)
  std::vector<uint8_t> tx_type;
  std::vector<uint8_t> cbf;
  std::vector<uint8_t> deblock_bs_v;
  std::vector<uint8_t> deblock_bs_h;
  std::vector<uint8_t> filter_idx;
};

/// FrameAnalysis.refs of a picture (chapter 06 §6.3.1 "RefSlot.name"): "L0[i]" / "L1[i]" for the lists of the first
/// slice segment, then "S<s>:L0[i]" / "S<s>:L1[i]" for each later slice segment s whose lists differ from every
/// earlier one (a segment with the same lists as an earlier one shares its slots).
class RefTable {
 public:
  struct Slot {
    std::string name;
    bool long_term = false;
    uint32_t usage_cells = 0;
  };

  explicit RefTable(const std::vector<SliceHeader>& slices);

  /// The refs[] index of entry @p ref_idx of list @p list of slice segment @p slice; -1 when there is none.
  [[nodiscard]] int8_t slot(std::size_t slice, int list, int ref_idx) const;
  void count(int8_t slot) {
    if (slot >= 0) {
      ++slots_.at(static_cast<std::size_t>(slot)).usage_cells;
    }
  }
  [[nodiscard]] const std::vector<Slot>& slots() const { return slots_; }

 private:
  // refs[] indices of an earlier segment of @p slices with the same lists as segment @p s; {-1, -1} when none.
  [[nodiscard]] std::array<int32_t, 2> shared_base(const std::vector<SliceHeader>& slices, std::size_t s) const;
  // Appends the slots of segment @p s (list sizes @p n); returns their refs[] indices.
  std::array<int32_t, 2> add_slots(const SliceHeader& sh, std::size_t s, const std::array<int32_t, 2>& n);

  std::vector<Slot> slots_;
  std::vector<std::array<int32_t, 2>> base_;  // per slice segment: refs[] index of L0[0] and L1[0]; -1 none
  std::vector<std::array<int32_t, 2>> size_;  // per slice segment: active entries of L0 and L1
};

/// Fills the per-cell vectors that come from libde265's per-4x4 metadata (everything except block_id and the
/// transform fields) and counts the reference usage in @p refs. @p d must have passed the size checks.
void fill_cells(GridCells& g, const BlockData& d, const PictureFormat& fmt, RefTable& refs);

/// Serialises @p g with every vector present.
flatbuffers::Offset<ovt::fb::CellGrid> build_grid(flatbuffers::FlatBufferBuilder& fbb, uint32_t cells_w,
                                                  uint32_t cells_h, const GridCells& g);

/// FrameAnalysis.qp_hist: 256 bins of grid.qp + 128 (chapter 06 §6.3.1).
flatbuffers::Offset<flatbuffers::Vector<uint32_t>> build_qp_hist(flatbuffers::FlatBufferBuilder& fbb,
                                                                 const GridCells& g);

/// RefSlot entries of @p refs.
flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ovt::fb::RefSlot>>> build_refs(
    flatbuffers::FlatBufferBuilder& fbb, const RefTable& refs);

}  // namespace ovt::de265

#endif  // OVT_PLUGIN_DE265_MAP_GRID_HPP
