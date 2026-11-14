// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// ovt::de265::Decoder drives one libde265 decoder context the way chapter 07 §7.6.2 "Decoder driving" describes
// (WI-2.163): push one NAL unit, decode until libde265 wants more input, copy every output picture into plugin-owned
// memory at once and release it back to libde265. Internal to the plugin; plugin.cpp wraps it in the ABI.
#ifndef OVT_PLUGIN_DE265_DECODER_HPP
#define OVT_PLUGIN_DE265_DECODER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"

struct de265_image;

namespace ovt::de265 {

/// A Diagnostic for FrameAnalysis.diagnostics; severity is an ovt.fb.Severity value.
struct Diag {
  uint8_t severity = 0;
  std::string code;
  std::string message;
};

/// One plane of a picture, copied out of libde265: the whole decoded picture (pic_width_in_luma_samples ×
/// pic_height_in_luma_samples, i.e. FrameInfo.coded_size), rows of width × sample_bytes bytes, little-endian.
struct Plane {
  std::vector<uint8_t> data;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bit_depth = 8;
  uint8_t sample_bytes = 1;
  bool is_signed = false;  // Residual (int16) and Coefficients (int32) planes
};

/// The planes of one ovt.fb.PlaneStage: Y, U, V (U and V empty for 4:0:0).
using StagePlanes = std::array<Plane, 3>;

/// Everything the plugin keeps of one output picture, or a diagnostics-only record (picture == false).
struct Frame {
  int32_t decode_index = -1;
  int64_t pts = 0;
  bool picture = true;
  // FrameInfo
  uint8_t frame_type = 7;  // ovt.fb.FrameType: 0 I, 1 P, 2 B, 7 Unknown
  bool is_rap = false;
  uint8_t temporal_id = 0;
  uint8_t layer_id = 0;
  int64_t poc = 0;
  int32_t coded_w = 0;
  int32_t coded_h = 0;
  int32_t crop_x = 0;
  int32_t crop_y = 0;
  int32_t crop_w = 0;
  int32_t crop_h = 0;
  uint8_t chroma_format = 1;
  uint8_t bit_depth_luma = 8;
  uint8_t bit_depth_chroma = 8;
  int16_t qp_base = 0;
  uint16_t slice_count = 0;
  std::array<Plane, 3> planes;  // Reconstruction; planes 1 and 2 are empty for 4:0:0
  // The patch-0003 stages (planes.hpp), keyed by ovt.fb.PlaneStage value; only stages the host has asked for.
  std::map<uint32_t, StagePlanes> snapshots;
  std::vector<Diag> diagnostics;
};

class Decoder {
 public:
  /// Creates a libde265 context with @p threads worker threads (none when < 1) and pushes the parameter sets of
  /// an hvcC record when @p codec_private is not NULL. OVT_E_CORRUPT for a malformed hvcC.
  static ovt_status create(const uint8_t* codec_private, size_t len, int threads, std::unique_ptr<Decoder>* out);

  Decoder() = default;  // unusable until create() has set it up; public only for std::make_unique
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) = delete;
  Decoder& operator=(Decoder&&) = delete;

  /// Pushes one NAL unit (no start code, emulation-prevention bytes kept) and decodes what it can.
  ovt_status push(const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts);
  /// End of input: decodes the rest and outputs every picture still in the reorder buffer.
  ovt_status flush();
  /// Moves the next finished frame (output order) into @p out; false when there is none. Resumes decoding when
  /// it was held back by the outstanding-frame limit.
  bool pop(Frame* out);

  /// Notes the ovt.fb.PlaneStage bits of a pull(). The first request for a patch-0003 stage (Prediction,
  /// Residual, PreDeblock, PreSaoOrCdef, Coefficients) turns libde265's plane snapshots on for every picture that
  /// starts decoding afterwards; requests add up for the decoder's lifetime (planes.hpp).
  void request_stages(uint32_t plane_stage_mask);

 private:
  ovt_status decode_loop();
  void collect_warnings();
  void drain_pictures();
  void copy_picture(const de265_image* img, Frame* frame);
  void add_diag(uint8_t severity, std::string code, std::string message);
  ovt_status push_nal(std::span<const uint8_t> unit, int32_t decode_index_hint, int64_t pts);
  ovt_status push_hvcc(std::span<const uint8_t> hvcc);

  void* ctx_ = nullptr;  // de265_decoder_context*
  std::deque<Frame> frames_;
  std::vector<Diag> pending_diags_;  // attached to the next frame that comes out
  std::map<int32_t, int64_t> pts_;   // decode index -> host pts (libde265's pts carries the decode index)
  bool flushed_ = false;
  bool held_back_ = false;        // decode_loop stopped at the outstanding-frame limit with input left
  uint32_t snapshot_stages_ = 0;  // patch-0003 stages requested so far
};

}  // namespace ovt::de265

#endif  // OVT_PLUGIN_DE265_DECODER_HPP
