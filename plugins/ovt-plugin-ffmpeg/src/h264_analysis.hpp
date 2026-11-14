// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::ffmpeg::H264Analysis drives one libavcodec h264 decoder the way chapter 07 §7.6.3 "Analysis decoder
// driving" describes (WI-2.169), or one hevc decoder for the optional ffmpeg_hevc plugin (WI-4.107): raw NAL units are
// prefixed with a start code and batched into one Annex B packet per decode_index_hint, the packet carries the hint in
// pts and opaque (AV_CODEC_FLAG_COPY_OPAQUE), and every frame libavcodec returns keeps the decode index of the packet
// that started it. Internal to the plugin; plugin.cpp wraps it in the ABI.
#ifndef OVT_PLUGIN_FFMPEG_H264_ANALYSIS_HPP
#define OVT_PLUGIN_FFMPEG_H264_ANALYSIS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "map_h264_blockparams.hpp"
#include "map_h264_sidedata.hpp"
#include "map_hevc_sidedata.hpp"
#include "ovt/abi/analysis_decoder_v1.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace ovt::ffmpeg {

/// One coded-size plane copied out of the AVFrame: rows of width × sample_bytes bytes, no padding, little-endian.
struct Plane {
  std::vector<uint8_t> data;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bit_depth = 8;
  uint8_t sample_bytes = 1;
};

/// Everything the plugin reports for one output frame.
struct Frame {
  int32_t decode_index = -1;
  int64_t pts = 0;
  uint8_t frame_type = 7;  // ovt.fb.FrameType: 0 I, 1 P, 2 B, 7 Unknown
  bool is_rap = false;
  int32_t coded_w = 0;
  int32_t coded_h = 0;
  int32_t crop_x = 0;
  int32_t crop_y = 0;
  int32_t crop_w = 0;
  int32_t crop_h = 0;
  uint8_t chroma_format = 1;  // 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4
  uint8_t bit_depth_luma = 8;
  uint8_t bit_depth_chroma = 8;
  std::array<Plane, 3> planes;  // filled only when pop() is asked for them; planes 1 and 2 are empty for 4:0:0
  SideData side;                // the grid, blocks, motion vectors and references mapped from the side data (WI-2.170)
  BlockParams blocks;           // AvcMb and the phase-4 grid fields, from the block params when present (WI-4.102)
  std::vector<Diag> diagnostics;
};

struct AVFrameDeleter {
  void operator()(AVFrame* f) const noexcept;
};

/// The libavcodec decoder an H264Analysis drives.
enum class Codec : uint8_t { H264, H265 };

class H264Analysis {
 public:
  /// As create() below, for @p codec: H265 takes an hvcC record (ISO/IEC 14496-15 8.3.3.1) in @p codec_private, whose
  /// VPS/SPS/PPS arrays are sent in front of the first access unit, and maps the side data with map_hevc_sidedata().
  static ovt_status create(Codec codec, const uint8_t* codec_private, size_t len, int threads,
                           std::unique_ptr<H264Analysis>* out);
  /// Opens a libavcodec h264 decoder with @p threads slice threads (one when < 1; no frame threads, see
  /// SPEC-ASSUMPTION(#302) in h264_analysis.cpp). An avcC record in
  /// @p codec_private is never given to libavcodec as extradata (that switches it to length-prefixed input);
  /// its SPS/PPS are sent as Annex B units in front of the first access unit instead. OVT_E_CORRUPT for a
  /// malformed avcC, OVT_E_UNSUPPORTED when the FFmpeg build has no h264 decoder.
  static ovt_status create(const uint8_t* codec_private, size_t len, int threads, std::unique_ptr<H264Analysis>* out) {
    return create(Codec::H264, codec_private, len, threads, out);
  }

  H264Analysis() = default;  // unusable until create() has set it up; public only for std::make_unique
  ~H264Analysis();
  H264Analysis(const H264Analysis&) = delete;
  H264Analysis& operator=(const H264Analysis&) = delete;
  H264Analysis(H264Analysis&&) = delete;
  H264Analysis& operator=(H264Analysis&&) = delete;

  /// Adds one NAL unit (no start code, emulation-prevention bytes kept) to the access unit of
  /// @p decode_index_hint. A new hint first sends the previous access unit to the decoder.
  /// The status concerns the PREVIOUS access unit: OVT_E_CORRUPT means libavcodec rejected it (or failed while
  /// decoding it), although @p unit itself was accepted and is part of the new access unit. The Diagnostic
  /// travels with the next frame out. Callers must not push @p unit again.
  ovt_status push(const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts);
  /// End of input: sends the last access unit, drains the decoder (avcodec_send_packet(NULL)). The decode-index to
  /// pts table is emptied once the decoder is drained.
  ovt_status flush();
  /// Moves the next frame (libavcodec output order) into @p out, with its planes copied when @p want_planes;
  /// false when there is none.
  bool pop(Frame* out, bool want_planes);

  /// Decode indices still waiting for the pts of their frame (tests: the table stays bounded).
  [[nodiscard]] size_t pending_pts() const { return pts_.size(); }
  /// Most entries pts_ keeps; the oldest entries go first. libavcodec reorders by at most 16 frames plus
  /// one per frame thread, so no live entry is near the limit.
  static constexpr size_t kMaxPendingPts = 256;
  /// Most Diagnostics kept for the next frame out; later ones are dropped.
  static constexpr size_t kMaxPendingDiags = 64;
  /// Attempts at one avcodec_send_packet() while the decoder answers EAGAIN (its output is read in between).
  static constexpr int kMaxSendAttempts = 4;

 private:
  struct Pending {
    Frame info;
    std::unique_ptr<AVFrame, AVFrameDeleter> frame;
  };
  ovt_status send_access_unit();
  ovt_status send(const AVPacket* pkt);
  ovt_status receive_all();
  void add_diag(uint8_t severity, std::string code, std::string message);

  Codec codec_ = Codec::H264;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;             // receive buffer
  std::vector<uint8_t> parameter_sets_;  // Annex B parameter sets from avcC/hvcC, sent with the first access unit
  std::vector<uint8_t> au_;              // Annex B bytes of the access unit being collected
  int32_t au_hint_ = 0;
  bool has_au_ = false;
  bool flushed_ = false;
  std::deque<Pending> frames_;
  std::vector<Diag> pending_diags_;  // attached to the next frame that comes out
  std::map<int32_t, int64_t> pts_;   // decode index -> host pts; AUs libavcodec drops leave entries (bounded)
  std::deque<int32_t> pts_order_;    // decode indices in push order: the oldest go first when pts_ is full
};

}  // namespace ovt::ffmpeg

#endif  // OVT_PLUGIN_FFMPEG_H264_ANALYSIS_HPP
