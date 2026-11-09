// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::demux::Demuxer, the libavformat demux wrapper (Role B, chapter 08 §8.7, WI-1.230). Internal to
// libovt-core: the C API (ovt/decode.h) wraps it, and no FFmpeg type crosses the C ABI.
//
// WI-1.230 delivers open / streams / select_streams / read_packet / seek_to_time / raw and the custom
// AVIOContext over ovt::io::RandomAccessFile (§8.7.2). Packet::units and unit_bytes() arrive with the unit
// splitter (WI-1.231); Packet::sample_index, seek_to_sample() and match_tracks() with WI-1.232.
//
// Thread-safety: one Demuxer per thread (backbone §0.7.2). The RandomAccessFile may be shared.

#ifndef OVT_DEMUX_DEMUXER_H
#define OVT_DEMUX_DEMUXER_H

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/result.hpp"
#include "ovt_container_generated.h"
#include "ovt_model_generated.h"

namespace ovt::demux {

// SPEC-ASSUMPTION(#316): chapter 07 §7.1.6 defines ovt::Result<void> but no ovt::Status; §8.7.1 uses the name
// for a status-only result.
using Status = ovt::Result<void>;

struct StreamInfo {
  int avstream_index = -1;                         // AVStream::index
  int container_track_id = -1;                     // AVStream::id (track_ID / TrackNumber / PID), -1 if none
  ovt::fb::Codec codec = ovt::fb::Codec::Unknown;  // codec_from_av()
  AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
  AVRational time_base{0, 1};
  int64_t nb_frames = 0;                      // 0 if unknown
  int64_t duration = AV_NOPTS_VALUE;          // in time_base; AV_NOPTS_VALUE if unknown
  std::string codec_private_fourcc = "none";  // "avcC","hvcC","av1C","vpcC","annexb","none" (§8.7.6)
  std::vector<uint8_t> codec_private;         // §8.7.6; empty for "annexb", "none" and "vpcC" (Role A supplies vpcC)
  uint8_t nal_length_size = 0;                // 1,2,4 for length-prefixed streams; 0 for Annex B; 0 for AV1
  bool length_prefixed = false;               // true for ISOBMFF/Matroska AVC/HEVC; false for Annex B (TS, .264)
};

struct Packet {
  int avstream_index = -1;
  int64_t sample_index = -1;     // container sample index; always -1 until WI-1.232 (§8.7.3)
  int64_t pts = AV_NOPTS_VALUE;  // AVPacket values, stream time_base
  int64_t dts = AV_NOPTS_VALUE;
  int64_t duration = 0;
  int64_t file_pos = -1;          // AVPacket::pos, -1 unknown
  bool keyframe = false;          // AV_PKT_FLAG_KEY
  std::span<const uint8_t> data;  // valid until the next read_packet()/seek_to_time()/destructor
};

// AVCodecID → ovt::fb::Codec (§8.7.1): H264, HEVC, AV1, VP9, VVC (H266), MPEG2VIDEO, else Unknown.
[[nodiscard]] ovt::fb::Codec codec_from_av(AVCodecID id) noexcept;

// The libavformat demuxer name for a ContainerTree.format value (§8.7.2 rule 5). "annexb" and "" return ""
// (libavformat probes); also accepted: the libavformat names "h264", "hevc" and "av1" for the Annex B cases.
// std::nullopt: no mapping (unknown name, or "raw", which needs a RawVideoFormat: WI-1.229).
[[nodiscard]] std::optional<std::string_view> libavformat_name(std::string_view hint_format) noexcept;

namespace detail {
class AvioAdapter;
}

class Demuxer {
 public:
  // Opens through a custom AVIOContext over the RandomAccessFile (§8.7.2). hint_format: ContainerTree.format or
  // "" (lets libavformat probe). `tree` is nullable and kept for the sample_index mapping of WI-1.232. Never
  // throws. Errors: OVT_E_INVALID_ARG (null file, unknown hint), OVT_E_UNSUPPORTED (no demuxer for the input or
  // the hint), OVT_E_CORRUPT (libavformat rejects the bytes), OVT_E_IO (the file failed), OVT_E_NOMEM.
  static ovt::Result<std::unique_ptr<Demuxer>> open(std::shared_ptr<ovt::io::RandomAccessFile> file,
                                                    std::string_view hint_format,
                                                    const ovt::fb::ContainerTree* tree = nullptr);
  ~Demuxer();
  Demuxer(const Demuxer&) = delete;
  Demuxer& operator=(const Demuxer&) = delete;
  Demuxer(Demuxer&&) = delete;
  Demuxer& operator=(Demuxer&&) = delete;

  [[nodiscard]] const std::vector<StreamInfo>& streams() const noexcept { return streams_; }
  // Selects the streams whose packets read_packet() returns (others are discarded with AVDISCARD_ALL). All
  // streams are selected after open(). Indices outside streams() are ignored.
  void select_streams(std::span<const int> avstream_indices);
  // The next packet of a selected stream; std::nullopt at end of stream. Never throws; libavformat errors map
  // to OVT_E_IO / OVT_E_CORRUPT / OVT_E_NOMEM.
  // SPEC-ASSUMPTION(#316): §8.7.1 names a "Status::EndOfStream" that ovt_status does not have; end of stream
  // is an ok Result holding std::nullopt.
  ovt::Result<std::optional<Packet>> read_packet();
  // Seeks so that the next packet of avstream_index is the last sync sample at or before `ts` (stream
  // time_base) when `backward`, else the first at or after it (§8.7.5). OVT_E_INVALID_ARG for a bad index,
  // OVT_E_UNSUPPORTED when libavformat cannot seek the input, OVT_E_IO when the file failed.
  Status seek_to_time(int avstream_index, int64_t ts, bool backward = true);
  // For the playback decoder (chapter 7); never exposed through the C API.
  AVFormatContext* raw() noexcept { return fmt_; }

 private:
  Demuxer();

  std::shared_ptr<ovt::io::RandomAccessFile> file_;
  std::unique_ptr<detail::AvioAdapter> avio_;
  AVFormatContext* fmt_ = nullptr;
  AVPacket* pkt_ = nullptr;
  const ovt::fb::ContainerTree* tree_ = nullptr;
  std::vector<StreamInfo> streams_;
  std::vector<bool> selected_;
};

namespace detail {

// The custom AVIOContext of §8.7.2 (avio_adapter.cpp): read-only, seekable, over a RandomAccessFile.
class AvioAdapter {
 public:
  // nullptr when FFmpeg cannot allocate the buffer or the context.
  static std::unique_ptr<AvioAdapter> create(std::shared_ptr<ovt::io::RandomAccessFile> file);
  // Frees ctx->buffer (libavformat may have replaced the original one) and then the context. The owning
  // AVFormatContext must be closed first (avformat_close_input).
  ~AvioAdapter();
  AvioAdapter(const AvioAdapter&) = delete;
  AvioAdapter& operator=(const AvioAdapter&) = delete;
  AvioAdapter(AvioAdapter&&) = delete;
  AvioAdapter& operator=(AvioAdapter&&) = delete;

  [[nodiscard]] AVIOContext* context() const noexcept { return ctx_; }
  // True once a RandomAccessFile read threw (the callback returned AVERROR(EIO)).
  [[nodiscard]] bool io_failed() const noexcept { return io_failed_; }

  static constexpr int kBufferSize = 256 * 1024;

 private:
  explicit AvioAdapter(std::shared_ptr<ovt::io::RandomAccessFile> file) : file_(std::move(file)) {}
  static int read(void* opaque, uint8_t* buf, int size);
  static int64_t seek(void* opaque, int64_t offset, int whence);

  std::shared_ptr<ovt::io::RandomAccessFile> file_;
  AVIOContext* ctx_ = nullptr;
  uint64_t pos_ = 0;
  bool io_failed_ = false;
};

}  // namespace detail

}  // namespace ovt::demux

#endif  // OVT_DEMUX_DEMUXER_H
