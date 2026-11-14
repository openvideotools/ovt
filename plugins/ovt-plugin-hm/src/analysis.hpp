// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// The block data of one HM picture (chapter 07 §7.6.4 "Mapping via public TComDataCU getters", WI-4.105 part b):
// map_cu.cpp fills an Analysis from TComDataCU while HM still holds the picture; build_analysis() writes it into
// the FrameAnalysis on the host's thread. Free of HM headers.
#ifndef OVT_PLUGIN_HM_ANALYSIS_HPP
#define OVT_PLUGIN_HM_ANALYSIS_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt_model_generated.h"

class TComPic;  // HM, global namespace

namespace ovt::hm {

/// CellGrid vectors the plugin fills (ovt_AnalysisCaps.grid_fields): all but partition_id (no PartitionInfo yet),
/// bits_x8 (HM fills TComDataCU::getTotalBits() only when encoding) and deblock_bs (computed transiently).
constexpr uint32_t kGridFields = 0xFFFFU & ~(OVT_GRID_PARTITION_ID | OVT_GRID_BITS_X8 | OVT_GRID_DEBLOCK_BS);

/// One Block of the tree (chapter 06 §6.3.2), id = its index in Analysis::blocks.
struct BlockRec {
  int32_t parent = -1;
  std::vector<uint32_t> children;
  fb::BlockKind kind = fb::BlockKind::Other;
  fb::Rect rect;
  uint8_t level = 0;
  uint8_t partition_type = 255;
  fb::PredKind pred = fb::PredKind::None;
  bool skip = false;
  bool merge = false;
  int16_t qp = 0;
  std::string ext_type;
  std::vector<uint8_t> ext;  // nested HevcCtu / HevcCu / HevcPu / HevcTu
};

struct MvRec {
  uint32_t block_id = 0;  // the PU
  uint8_t list = 0;
  int8_t ref = -1;
  fb::Rect rect;
  int16_t mv_x = 0;  // 1/16 luma samples
  int16_t mv_y = 0;
  int16_t mvd_x = 0;
  int16_t mvd_y = 0;
  std::string predictor;
  uint16_t flags = 0;
};

struct RefRec {
  int32_t decode_index = -1;
  std::string name;
  bool long_term = false;
  uint32_t usage_cells = 0;
};

/// The CellGrid vectors, one value per 4x4 cell in raster order.
struct Grid {
  uint32_t w4 = 0;
  uint32_t h4 = 0;
  std::vector<uint32_t> block_id;
  std::vector<uint8_t> pred;
  std::vector<int16_t> qp, intra_y, intra_c, mv0_x, mv0_y, mv1_x, mv1_y;
  std::vector<int8_t> ref0, ref1;
  std::vector<uint8_t> tx_log2, tx_type, cbf, filter_idx;

  void resize(uint32_t w, uint32_t h);
};

struct Analysis {
  bool valid = false;
  Grid grid;
  std::vector<BlockRec> blocks;
  std::vector<MvRec> mvs;
  std::vector<RefRec> refs;
};

/// Decode indices of the pictures the plugin exported, for RefSlot.frame (cleared when HM destroys its pictures).
using PicIndex = std::unordered_map<const TComPic*, int32_t>;

/// Fills @p out from the CTUs of @p pic (reconstructed and loop-filtered). Call with HM's z-scan tables built for
/// the picture's CTU size, before HM releases or reuses it.
void map_cu(TComPic& pic, const PicIndex& index, Analysis* out);

/// The FlatBuffers parts of an Analysis.
struct MappedAnalysis {
  flatbuffers::Offset<fb::CellGrid> grid;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::Block>>> blocks;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::MotionVector>>> mvs;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::RefSlot>>> refs;
};

/// Writes @p a into @p fbb; with `a.valid == false` only an empty grid of @p coded_w x @p coded_h samples.
MappedAnalysis build_analysis(flatbuffers::FlatBufferBuilder& fbb, const Analysis& a, int32_t coded_w, int32_t coded_h);

}  // namespace ovt::hm

#endif  // OVT_PLUGIN_HM_ANALYSIS_HPP
