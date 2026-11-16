// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MXF placeholder reader (chapter 08 §8.6.7, WI-4.211): libavformat (Role B) demuxes the file, OVT reads no KLV
// itself. The tree has one root node spanning the file with the "not implemented" description, and one TrackInfo
// per libavformat stream. Whether OVT ever writes its own KLV reader is an open owner decision
// (docs/adr/ADR-CNT-MXF.md); do not grow this file into one.

#include "container/mxf_placeholder.hpp"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "demux/demuxer.h"
#include "ovt_container_generated.h"

namespace ovt::container::mxf {
namespace {

constexpr uint64_t kUnknownDuration = std::numeric_limits<uint64_t>::max();  // TrackInfo.duration all-ones

std::span<AVStream* const> streams_of(const AVFormatContext* fmt) {
  if (fmt == nullptr || fmt->streams == nullptr) {
    return {};
  }
  return {fmt->streams, fmt->nb_streams};
}

fb::HandlerType handler_of(AVMediaType t) noexcept {
  switch (t) {
    case AVMEDIA_TYPE_VIDEO:
      return fb::HandlerType::Video;
    case AVMEDIA_TYPE_AUDIO:
      return fb::HandlerType::Audio;
    case AVMEDIA_TYPE_SUBTITLE:
      return fb::HandlerType::Subtitle;
    default:  // data (ANC, timecode) and attachments: MXF has no hdlr to name them
      return fb::HandlerType::Unknown;
  }
}

// The ISOBMFF handler vocabulary for the three media types it shares with libavformat; empty otherwise.
const char* handler_fourcc_of(AVMediaType t) noexcept {
  switch (t) {
    case AVMEDIA_TYPE_VIDEO:
      return "vide";
    case AVMEDIA_TYPE_AUDIO:
      return "soun";
    case AVMEDIA_TYPE_SUBTITLE:
      return "subt";
    default:
      return "";
  }
}

uint32_t clamp_u32(int64_t v) noexcept {
  if (v <= 0) {
    return 0;
  }
  return v > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(v);
}

// TrackInfo timing from the stream time_base: the timescale is time_base.den and the duration is counted in
// 1/den ticks (duration * time_base.num), so 1001/30000 streams keep exact values.
struct Timing {
  uint32_t timescale = 0;
  uint64_t duration = kUnknownDuration;
  double seconds = -1.0;
};

Timing timing_of(const demux::StreamInfo& s) noexcept {
  Timing t;
  if (s.time_base.num <= 0 || s.time_base.den <= 0) {
    return t;
  }
  t.timescale = static_cast<uint32_t>(s.time_base.den);
  if (s.duration == AV_NOPTS_VALUE || s.duration < 0 ||
      static_cast<uint64_t>(s.duration) > kUnknownDuration / static_cast<uint64_t>(s.time_base.num) - 1) {
    return t;
  }
  t.duration = static_cast<uint64_t>(s.duration) * static_cast<uint64_t>(s.time_base.num);
  t.seconds = static_cast<double>(t.duration) / static_cast<double>(t.timescale);
  return t;
}

flatbuffers::Offset<fb::TrackInfo> track_info(flatbuffers::FlatBufferBuilder& fbb, uint32_t index,
                                              const demux::StreamInfo& s, const AVCodecParameters* par) {
  const Timing timing = timing_of(s);
  const auto handler_fourcc = fbb.CreateString(handler_fourcc_of(s.media_type));
  std::vector<flatbuffers::Offset<fb::CodecPrivate>> privs;
  if (!s.codec_private.empty()) {
    privs.push_back(fb::CreateCodecPrivate(fbb, fbb.CreateString(s.codec_private_fourcc),
                                           fbb.CreateVector(s.codec_private.data(), s.codec_private.size()), 0));
  }
  const auto codec_private = fbb.CreateVector(privs);
  flatbuffers::Offset<flatbuffers::Vector<uint32_t>> pixel_aspect;
  if (par != nullptr && par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
    const std::vector<uint32_t> pasp = {static_cast<uint32_t>(par->sample_aspect_ratio.num),
                                        static_cast<uint32_t>(par->sample_aspect_ratio.den)};
    pixel_aspect = fbb.CreateVector(pasp);
  }

  fb::TrackInfoBuilder tb(fbb);
  tb.add_index(index);
  // AVStream::id is the MXF track ID libavformat read from the track set; 0 when it set none.
  tb.add_track_id(s.container_track_id < 0 ? 0U : static_cast<uint32_t>(s.container_track_id));
  tb.add_handler(handler_of(s.media_type));
  tb.add_handler_fourcc(handler_fourcc);
  tb.add_codec(s.codec);
  tb.add_sample_entry_count(1);
  tb.add_timescale(timing.timescale);
  tb.add_duration(timing.duration);
  tb.add_duration_seconds(timing.seconds);
  tb.add_enabled(true);
  tb.add_in_movie(true);
  if (par != nullptr) {
    tb.add_width(par->width);
    tb.add_height(par->height);
    tb.add_coded_width(clamp_u32(par->width));
    tb.add_coded_height(clamp_u32(par->height));
    tb.add_channel_count(clamp_u32(par->ch_layout.nb_channels));
    tb.add_sample_rate(par->sample_rate);
    tb.add_sample_size(clamp_u32(par->bits_per_coded_sample));
    tb.add_bitrate_avg_bps(clamp_u32(par->bit_rate));
  }
  if (!pixel_aspect.IsNull()) {
    tb.add_pixel_aspect(pixel_aspect);
  }
  tb.add_sample_count(s.nb_frames > 0 ? static_cast<uint64_t>(s.nb_frames) : 0U);
  tb.add_codec_private(codec_private);
  tb.add_avstream_index(s.avstream_index);
  return tb.Finish();
}

std::vector<uint8_t> build_tree(uint64_t file_size, demux::Demuxer& demuxer) {
  flatbuffers::FlatBufferBuilder fbb;
  const fb::ByteSpan root_span(0, file_size);
  const auto root = fb::CreateBoxNode(fbb, fbb.CreateString("MXF"), 0, &root_span, 0, -1, -1, 0, nullptr, 0,
                                      fbb.CreateString("chapter 08 §8.6.7"),
                                      fbb.CreateString(kPlaceholderDescription.data(), kPlaceholderDescription.size()));

  const std::span<AVStream* const> avs = streams_of(demuxer.raw());
  std::vector<flatbuffers::Offset<fb::TrackInfo>> tracks;
  uint32_t index = 0;
  for (const demux::StreamInfo& s : demuxer.streams()) {
    const AVStream* st = s.avstream_index >= 0 && static_cast<size_t>(s.avstream_index) < avs.size()
                             ? avs[static_cast<size_t>(s.avstream_index)]
                             : nullptr;
    tracks.push_back(track_info(fbb, index++, s, st == nullptr ? nullptr : st->codecpar));
  }
  fbb.Finish(
      fb::CreateContainerTree(fbb, fbb.CreateString("mxf"), fbb.CreateVector(&root, 1), fbb.CreateVector(tracks)));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

class MxfPlaceholderReader final : public Reader {
 public:
  MxfPlaceholderReader(std::vector<uint8_t> tree, int track_count)
      : tree_(std::move(tree)), track_count_(track_count) {}
  [[nodiscard]] int track_count() const noexcept override { return track_count_; }
  [[nodiscard]] std::vector<uint8_t> tree() const override { return tree_; }

 private:
  std::vector<uint8_t> tree_;
  int track_count_ = 0;
};

}  // namespace

std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, ovt_status* why) {
  // The demuxer wants shared ownership, the container API lends a reference. A non-owning alias is safe: the
  // demuxer is destroyed before this function returns, and it only calls the const read()/size() members.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  std::shared_ptr<io::RandomAccessFile> borrowed(std::shared_ptr<void>{}, const_cast<io::RandomAccessFile*>(&file));
  auto opened = demux::Demuxer::open(std::move(borrowed), "mxf");
  if (!opened.has_value()) {
    if (why != nullptr) {
      *why = opened.status();
    }
    return nullptr;
  }
  demux::Demuxer& demuxer = *opened.value();
  const int count = static_cast<int>(demuxer.streams().size());
  return std::make_unique<MxfPlaceholderReader>(build_tree(file.size(), demuxer), count);
}

}  // namespace ovt::container::mxf
