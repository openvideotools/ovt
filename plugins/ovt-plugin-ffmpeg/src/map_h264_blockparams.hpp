// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV_FRAME_DATA_VIDEO_BLOCK_PARAMS (the OVT FFmpeg patch 0001, WI-4.100/4.101) -> the H.264 extension table AvcMb and
// the grid fields phase 1 leaves empty (chapter 07 §7.6.3 "AvcMb mapping", WI-4.102). It refines the SideData of
// map_h264_sidedata(): true ref_idx slots instead of the L0/L1 pseudo slots, mb_type-based prediction kinds, the
// signalled mvd of each vector. Without the side data (an unpatched FFmpeg) nothing changes: phase 1 stands.
#ifndef OVT_PLUGIN_FFMPEG_MAP_H264_BLOCKPARAMS_HPP
#define OVT_PLUGIN_FFMPEG_MAP_H264_BLOCKPARAMS_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "map_h264_sidedata.hpp"

struct AVFrame;

namespace ovt::ffmpeg {

/// ovt.fb.PredKind values the block params add.
inline constexpr uint8_t kPredSkip = 3;
inline constexpr uint8_t kPredPcm = 5;

/// The ovt.fb.AvcMb fields of one macroblock (chapter 06 §6.4). Sentinels as in the schema: 255 unsigned, -1 signed,
/// -32768 for an mvd that is not signalled; an empty vector is "not present".
struct AvcMbInfo {
  bool decoded = false;  // false: the macroblock was not decoded (mb_type_class 255); only mb_addr is meaningful
  uint32_t mb_addr = 0;
  uint8_t mb_type_raw = 255;
  uint8_t mb_type = 255;  // ovt.fb.AvcMbType (unified)
  uint8_t mb_skip_flag = 255;
  uint8_t mb_field_decoding_flag = 255;
  uint8_t transform_size_8x8_flag = 255;
  uint8_t coded_block_pattern = 255;
  int8_t mb_qp_delta = 0;
  int8_t qp_y = -1;
  int8_t qpc_cb = -1;
  int8_t qpc_cr = -1;
  std::vector<uint8_t> sub_mb_type;         // 4 entries (ovt.fb.AvcSubMbType) for P_8x8 / B_8x8
  std::vector<uint8_t> intra4x4_pred_mode;  // 16 entries, luma4x4BlkIdx order
  std::vector<uint8_t> intra8x8_pred_mode;  // 4 entries
  uint8_t intra16x16_pred_mode = 255;
  uint8_t intra_chroma_pred_mode = 255;
  std::array<std::vector<int8_t>, 2> ref_idx;  // [list]: 4 entries (per 8x8) for inter macroblocks
  std::array<std::vector<int16_t>, 2> mvd_x;   // [list]: 16 entries (luma4x4BlkIdx order), quarter samples
  std::array<std::vector<int16_t>, 2> mvd_y;
  std::vector<uint8_t> pred_flag;   // 16 entries: bit0 predFlagL0, bit1 predFlagL1
  std::vector<uint8_t> direct_8x8;  // 4 entries in B slices
  uint16_t slice_index = 0;
  bool skip = false;    // Block.skip
  bool direct = false;  // Block.merge_or_direct
};

/// One slice of the picture: FrameAnalysis.partitions[index] (kind Slice).
struct SliceSpan {
  uint32_t first_mb = 0;
  uint32_t mb_count = 0;
  Box rect;                  // bounding box of its macroblocks
  uint8_t slice_type = 255;  // H.264 slice_type 0 P, 1 B, 2 I inferred from the macroblock types
};

/// What the block params add to a frame; `valid` is false (everything empty) when the frame carries none.
struct BlockParams {
  bool valid = false;
  std::vector<AvcMbInfo> mbs;          // raster order, one per SideData::mbs entry
  std::vector<uint16_t> partition_id;  // per cell: the slice index
  std::vector<int16_t> intra_mode_y;   // per cell, chapter 06 §6.3.1 numbering
  std::vector<int16_t> intra_mode_c;
  std::vector<uint8_t> tx_log2;  // per cell: tx_log2_w == tx_log2_h
  std::vector<uint8_t> cbf;
  std::vector<SliceSpan> slices;
  std::array<uint8_t, 2> ref_slots{};  // number of "L0[i]" / "L1[i]" slots; refs[] = L0[0..] then L1[0..]
  std::vector<uint32_t> usage_cells;   // per slot
};

/// The unified AvcMbType of an AVH264MacroblockParams mb_type_class (FFmpeg patch numbering), the prediction
/// directions of its 8x8 partitions (bit0 L0, bit1 L1), its Intra16x16PredMode and coded_block_pattern; 255 when the
/// class is unknown or the directions do not form a Table 7-14 type.
uint8_t avc_mb_type(int mb_type_class, const std::array<uint8_t, 4>& pred_dir, int intra16x16_pred_mode, int cbp);

/// The unified AvcSubMbType of one 8x8 partition of a P_8x8 (@p b_slice false) or B_8x8 macroblock from the patch's
/// sub_mb_type (bits 0-3 shape 8x8/8x4/4x8/4x4, bit 4 direct) and its prediction direction.
uint8_t avc_sub_mb_type(bool b_slice, int sub_mb_type, int pred_dir);

/// Maps the block params attached to @p frame onto @p side (which map_h264_sidedata() filled from the same frame):
/// prediction kinds, MotionVector.ref (true slots), mvd and field flags, the grid's ref0/ref1 and the slot usage.
/// Removes the ANA-H264-0002 "n/a" diagnostic from @p diags. Leaves both untouched and returns valid = false when the
/// frame has no block params or they do not cover the macroblocks of @p side.
BlockParams map_h264_blockparams(const AVFrame* frame, int bit_depth_chroma, SideData* side, std::vector<Diag>* diags);

/// True when this build's FFmpeg headers have the block params (AV_CODEC_EXPORT_DATA_VIDEO_BLOCK_PARAMS).
bool block_params_available();

}  // namespace ovt::ffmpeg

#endif  // OVT_PLUGIN_FFMPEG_MAP_H264_BLOCKPARAMS_HPP
