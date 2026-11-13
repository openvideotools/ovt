// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// ovt::aom::Decoder drives one libaom AV1 decoder in inspection mode, as chapter 07 §7.6.1 "Decoder driving"
// describes (WI-2.155). The host pushes one OBU per call; the OBUs of a temporal unit are collected and decoded
// when the next OBU_TEMPORAL_DELIMITER arrives or at flush(), one frame per aom_codec_decode() call, with
// decode_index assigned by position in the temporal unit. Internal to the plugin; plugin.cpp wraps it in the ABI.
#ifndef OVT_PLUGIN_AOM_DECODER_HPP
#define OVT_PLUGIN_AOM_DECODER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "block_data.hpp"
#include "ovt/abi/analysis_decoder_v1.h"

namespace ovt::aom {

/// A Diagnostic for FrameAnalysis.diagnostics; severity is an ovt.fb.Severity value.
struct Diag {
  uint8_t severity = 0;
  std::string code;
  std::string message;
};

/// One plane of the output image, copied out of libaom: rows of width × sample_bytes bytes, no padding,
/// little-endian 16-bit samples when sample_bytes == 2.
struct Plane {
  std::vector<uint8_t> data;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bit_depth = 8;
  uint8_t sample_bytes = 1;
};

/// Everything the plugin reports for one frame that had tile data (shown or hidden). show_existing_frame
/// headers produce no Frame: the host synthesises their entry (chapter 07 §7.5.3 A0).
struct Frame {
  int32_t decode_index = -1;
  int64_t pts = 0;
  uint8_t frame_type = 7;  // ovt.fb.FrameType: 3 Key, 4 Inter, 5 IntraOnly, 6 Switch, 7 Unknown
  bool shown = false;
  int16_t qp_base = 0;  // base_q_idx
  int32_t coded_w = 0;  // FrameWidth × FrameHeight, before super-resolution (chapter 06 §6.2.1 rule 2)
  int32_t coded_h = 0;
  int32_t render_w = 0;  // RenderWidth × RenderHeight: FrameInfo.crop at (0,0) (chapter 06 §6.2.1 rule 3)
  int32_t render_h = 0;
  uint8_t chroma_format = 1;  // 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4
  uint8_t bit_depth = 8;
  std::array<Plane, 3> planes;  // Reconstruction of shown frames; empty for hidden frames and 4:0:0 chroma
  BlockData blocks;             // libaom's MI grid and the frame-header values its mapping needs (WI-2.156)
  std::vector<Diag> diagnostics;
};

class Decoder {
 public:
  /// Opens a libaom AV1 decoder with @p threads worker threads (one when < 1; always one when libaom is built with
  /// CONFIG_ACCOUNTING, whose symbol recording is not thread-safe) and registers the inspection
  /// callback. OVT_E_PLUGIN with @p error = "libaom built without CONFIG_INSPECTION" when libaom refuses the
  /// callback; OVT_E_PLUGIN with libaom's message when the decoder cannot be opened.
  static ovt_status create(int threads, std::unique_ptr<Decoder>* out, std::string* error);

  Decoder() = default;  // unusable until create() has set it up; public only for std::make_unique
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) = delete;
  Decoder& operator=(Decoder&&) = delete;

  /// Adds one OBU (header, obu_size field and payload) with the decode index of the frame it belongs to. A
  /// temporal delimiter first decodes the temporal unit collected so far, so a decode error of that unit is
  /// returned here (OVT_E_CORRUPT, with a Diagnostic on the next frame out). OVT_E_CORRUPT for an OBU without
  /// obu_has_size_field or whose obu_size does not match @p len. OVT_E_PLUGIN "frame count mismatch at decode
  /// index %d" when the frames libaom found in the previous temporal unit do not end where this hint starts.
  ovt_status push(const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts);
  /// End of input: decodes the last temporal unit and drains libaom.
  ovt_status flush();
  /// Moves the next frame (decode order) into @p out; false when there is none.
  bool pop(Frame* out);
  /// The message of the last OVT_E_PLUGIN, for logs and tests.
  [[nodiscard]] const std::string& last_error() const { return last_error_; }

  struct Impl;  // libaom state; defined in decoder.cpp so that this header needs no libaom header

 private:
  struct FrameObu {
    size_t offset = 0;  // of the OBU in tu_
    int64_t pts = 0;
  };
  ovt_status decode_temporal_unit();
  void add_diag(uint8_t severity, std::string code, std::string message);

  std::unique_ptr<Impl> impl_;
  std::vector<uint8_t> tu_;          // the OBUs of the temporal unit being collected
  std::vector<FrameObu> tu_frames_;  // OBU_FRAME and OBU_FRAME_HEADER units of tu_, in order
  int32_t tu_first_hint_ = -1;       // decode_index_hint of the first frame OBU of tu_
  int32_t expected_next_ = -1;       // tu_first_hint_ + frames handled, after a decoded temporal unit; -1 unknown
  std::deque<Frame> frames_;
  std::vector<Diag> pending_diags_;  // attached to the next frame that comes out
  std::string last_error_;
};

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_DECODER_HPP
