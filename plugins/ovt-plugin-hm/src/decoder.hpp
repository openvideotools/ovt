// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// ovt::hm::Decoder drives one HM TDecTop as chapter 07 §7.6.4 "Decoder driving" describes (WI-4.105) and copies every
// finished picture into plugin-owned memory. plugin.cpp wraps it in the ABI; HM's headers stay in decoder.cpp.
#ifndef OVT_PLUGIN_HM_DECODER_HPP
#define OVT_PLUGIN_HM_DECODER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"

namespace ovt::hm {

/// A Diagnostic for FrameAnalysis.diagnostics; severity is an ovt.fb.Severity value.
struct Diag {
  uint8_t severity = 0;
  std::string code;
  std::string message;
};

/// One plane of a picture, copied out of HM: the whole decoded picture (pic_width_in_luma_samples ×
/// pic_height_in_luma_samples, i.e. FrameInfo.coded_size), rows of width × sample_bytes bytes, little-endian.
struct Plane {
  std::vector<uint8_t> data;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bit_depth = 8;
  uint8_t sample_bytes = 1;
};

/// Everything the plugin keeps of one decoded picture, or a diagnostics-only record (picture == false).
struct Frame {
  int32_t decode_index = -1;
  int64_t pts = 0;
  bool picture = true;
  uint8_t frame_type = 7;  // ovt.fb.FrameType: 0 I, 1 P, 2 B, 7 Unknown
  bool shown = true;       // PicOutputFlag
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
  std::array<Plane, 3> planes;  // planes 1 and 2 are empty for 4:0:0
  std::vector<Diag> diagnostics;
};

class Decoder {
 public:
  /// Creates an HM decoder and pushes the parameter sets of an hvcC record when @p codec_private is not NULL.
  /// OVT_E_CORRUPT for a malformed hvcC. HM decodes on the calling thread only; @p threads is ignored.
  static ovt_status create(const uint8_t* codec_private, size_t len, std::unique_ptr<Decoder>* out);

  Decoder();
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) = delete;
  Decoder& operator=(Decoder&&) = delete;

  /// Pushes one NAL unit (no start code, emulation-prevention bytes kept) and decodes it.
  ovt_status push(const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts);
  /// End of input: loop-filters and outputs the picture still being decoded.
  ovt_status flush();
  /// Moves the next finished frame (decode order) into @p out; false when there is none.
  bool pop(Frame* out);

  struct Impl;  // HM state; defined in decoder.cpp

 private:
  ovt_status push_hvcc(std::span<const uint8_t> hvcc);

  std::unique_ptr<Impl> impl_;
};

}  // namespace ovt::hm

#endif  // OVT_PLUGIN_HM_DECODER_HPP
