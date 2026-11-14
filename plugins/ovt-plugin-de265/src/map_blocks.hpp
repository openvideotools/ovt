// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The per-picture libde265 block metadata that the chapter 07 §7.6.2 mapping table reads (the rows available with
// patch 0001; WI-2.164). The decoder copies it out of libde265 through the patch-0001 accessors before the picture
// goes back to libde265; map_grid.cpp turns it into FlatBuffers parts of the FrameAnalysis on the host's thread.
// Free of libde265 headers so that the unit tests can build BlockData by hand.
#ifndef OVT_PLUGIN_DE265_MAP_BLOCKS_HPP
#define OVT_PLUGIN_DE265_MAP_BLOCKS_HPP

#include <array>
#include <cstdint>
#include <vector>

namespace ovt::de265 {

/// libde265 PredMode (slice.h): the numbering of de265_internals_cb.PredMode.
enum class De265PredMode : uint8_t { Intra = 0, Inter = 1, Skip = 2 };

/// de265_internals_cb of one MinCb unit (same fields and meaning). log2CbSize 0 = not decoded.
struct CbInfo {
  uint8_t log2CbSize = 0;
  uint8_t PartMode = 0;
  uint8_t ctDepth = 0;
  uint8_t PredMode = 0;
  uint8_t pcm_flag = 0;
  uint8_t cu_transquant_bypass = 0;
  int16_t QP_Y = 0;  // int8 in libde265
};

/// de265_internals_pb of one 4x4 unit (MVs in quarter samples).
struct PbInfo {
  std::array<uint8_t, 2> predFlag{};
  std::array<int8_t, 2> refIdx{-1, -1};
  std::array<int16_t, 2> mv_x{};
  std::array<int16_t, 2> mv_y{};
  std::array<int32_t, 2> refPOC{};
};

/// de265_internals_sao of one CTB.
struct SaoInfo {
  std::array<uint8_t, 3> SaoTypeIdx{};
  std::array<int8_t, 12> SaoOffsetVal{};  // [cIdx * 4 + i] = SaoOffsetVal[cIdx][i + 1] of H.265 7.4.9.3.2
  std::array<uint8_t, 3> SaoBandPosition{};
  std::array<uint8_t, 3> SaoEoClass{};
};

/// The fields of de265_internals_slice_header the mapping uses, for one slice segment.
struct SliceHeader {
  int32_t slice_type = 2;  // 0 B, 1 P, 2 I
  int32_t SliceAddrRs = 0;
  int32_t slice_qp_delta = 0;
  int32_t SliceQPY = 0;
  std::array<int32_t, 2> num_ref_idx_active{};
  std::array<std::array<int32_t, 16>, 2> RefPicList_POC{};
  std::array<std::array<int8_t, 16>, 2> RefPicList_LongTerm{};
  int32_t MaxNumMergeCand = 0;
  int32_t slice_deblocking_filter_disabled_flag = 0;
  int32_t slice_beta_offset_div2 = 0;
  int32_t slice_tc_offset_div2 = 0;
  int32_t slice_sao_luma_flag = 0;
  int32_t slice_sao_chroma_flag = 0;
};

/// libde265 deblk_info bits (image.h v1.0.15, DE265_INTERNALS_DEBLOCK_* of de265_internals.h).
constexpr uint8_t kDeblockBsMask = 0x03;
constexpr uint8_t kDeblockFlagHoriz = 1U << 5U;
constexpr uint8_t kDeblockPbEdgeHoriz = 1U << 7U;
/// libde265 tu_info bits (DE265_INTERNALS_TU_FLAG_*): bit d = split_transform_flag at trafoDepth d.
constexpr uint8_t kTuNonzeroCoeff = 1U << 7U;

/// Per-picture block metadata copied out of libde265. Every per-4x4 vector has cells_w * cells_h entries (the
/// CellGrid of the coded picture); cb has cbs_w * cbs_h MinCb units; the per-CTB vectors have ctbs_w * ctbs_h.
struct BlockData {
  bool valid = false;  // false without patch 0001 (a system libde265) or when the picture had no metadata
  int32_t ctb_log2 = 4;
  int32_t min_cb_log2 = 3;
  int32_t ctbs_w = 0;  // PicWidthInCtbsY
  int32_t ctbs_h = 0;
  int32_t cbs_w = 0;  // MinCb units
  int32_t cbs_h = 0;
  uint32_t cells_w = 0;  // 4x4 units
  uint32_t cells_h = 0;
  std::vector<CbInfo> cb;
  std::vector<PbInfo> pb;
  std::vector<uint8_t> intra_y;  // IntraPredModeY 0..34, 255 = not intra
  std::vector<uint8_t> intra_c;
  std::vector<uint8_t> tu;           // raw tu_info byte
  std::vector<uint8_t> deblk;        // raw deblk_info byte
  std::vector<uint8_t> bs_v;         // de265_internals_get_deblk_bS_v
  std::vector<uint16_t> ctb_slice;   // slice header index (= slice segment in decode order)
  std::vector<int32_t> ctb_addr_ts;  // CtbAddrInTs
  std::vector<int32_t> ctb_tile;     // TileId (tile scan)
  std::vector<SaoInfo> sao;
  std::vector<SliceHeader> slices;
};

/// Picture-level facts the mapping needs besides the block metadata.
struct PictureFormat {
  int32_t coded_w = 0;  // pic_width_in_luma_samples
  int32_t coded_h = 0;
  uint8_t chroma_format = 1;  // 0 = 4:0:0 (no chroma modes)
  uint8_t bit_depth_luma = 8;
};

}  // namespace ovt::de265

#endif  // OVT_PLUGIN_DE265_MAP_BLOCKS_HPP
