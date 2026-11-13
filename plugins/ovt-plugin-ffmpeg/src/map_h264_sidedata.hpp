// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libavcodec's H.264 side data -> OVT model (chapter 07 §7.6.3, WI-2.170): VIDEO_ENC_PARAMS gives macroblocks and QP,
// MOTION_VECTORS the vectors; intra/inter is inferred. Plain structs out (plugin.cpp builds the FlatBuffers).
#ifndef OVT_PLUGIN_FFMPEG_MAP_H264_SIDEDATA_HPP
#define OVT_PLUGIN_FFMPEG_MAP_H264_SIDEDATA_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct AVFrame;

namespace ovt::ffmpeg {

/// A Diagnostic for FrameAnalysis.diagnostics; severity is an ovt.fb.Severity value.
struct Diag {
  uint8_t severity = 0;
  std::string code;
  std::string message;
};

/// ovt.fb.PredKind values the mapping produces.
inline constexpr uint8_t kPredNone = 0;
inline constexpr uint8_t kPredIntra = 1;
inline constexpr uint8_t kPredInter = 2;

/// An axis-aligned rectangle in coded-picture luma samples.
struct Box {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  bool operator==(const Box&) const = default;
};

/// One macroblock, FrameAnalysis.blocks[1 + raster address] (kind MB).
struct MbBlock {
  Box rect;
  int16_t qp = -1;  // QP_Y without QpBdOffsetY, -1 when the frame carries no QP side data
  uint8_t pred = kPredNone;
};

/// One FrameAnalysis.mvs entry; `ref` is the pseudo reference slot, equal to `list` (L0 / L1), until
/// map_h264_blockparams() sets the true slot.
struct MbMotion {
  uint32_t block_id = 0;
  uint8_t list = 0;
  int8_t ref = 0;
  Box rect;
  int16_t mv_x = 0;  // 1/16 luma sample
  int16_t mv_y = 0;
  int16_t mvd_x = -32768;  // 1/16 luma sample; -32768 = n/a (phase 1 has no mvd; the block params fill it, WI-4.102)
  int16_t mvd_y = -32768;
  uint16_t flags = 0;  // ovt.fb.MotionVector.flags: bit0 bi-pred, bit4 field
};

/// The CellGrid vectors and block/MV/reference tables of one frame; `valid` is false (rest empty) without macroblocks.
struct SideData {
  bool valid = false;
  uint32_t cells_w = 0;  // ceil(coded_w / 4)
  uint32_t cells_h = 0;
  std::vector<uint32_t> block_id;  // per cell, row-major: 1 + raster macroblock address
  std::vector<uint8_t> pred;       // ovt.fb.PredKind
  std::vector<int16_t> qp;
  std::array<std::vector<int16_t>, 2> mv_x;  // [list]
  std::array<std::vector<int16_t>, 2> mv_y;
  std::array<std::vector<int8_t>, 2> ref;  // -1 none, else the list number (pseudo reference slot)
  std::array<uint32_t, 2> usage_cells{};   // RefSlot.usage_cells of "L0" / "L1"
  std::vector<MbBlock> mbs;                // raster order
  std::vector<MbMotion> mvs;               // canonical order: block_id, list, rect.y, rect.x
  std::vector<uint32_t> qp_hist;           // 256 bins of qp + 128; empty without QP side data
};

/// Maps the side data attached to @p frame. @p bit_depth_luma gives QpBdOffsetY = 6 * (bit_depth_luma - 8).
/// Diagnostics (ANA-H264-0001 interlaced, ANA-H264-0002 inferred intra, MDL-GEN-0003 clamped vectors, and a
/// code-less warning for vectors that do not fit the macroblock grid) are appended to @p diags.
SideData map_h264_sidedata(const AVFrame* frame, int bit_depth_luma, std::vector<Diag>* diags);

}  // namespace ovt::ffmpeg

#endif  // OVT_PLUGIN_FFMPEG_MAP_H264_SIDEDATA_HPP
