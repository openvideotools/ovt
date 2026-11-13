// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::demux::Demuxer (chapter 08 §8.7.1–§8.7.3, §8.7.5, §8.7.6; WI-1.230). OVT never inserts a bitstream
// filter here (§8.7.3): packets carry the container's bytes and offsets unchanged.

#include "demux/demuxer.h"

#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace ovt::demux {

namespace {

struct FormatAlias {
  std::string_view ovt;
  std::string_view lavf;
};

// §8.7.2 rule 5. "annexb" (H.264 or HEVC, decided by libavformat's probe) and "raw" are handled by the caller.
constexpr std::array kFormatAliases{
    FormatAlias{"isobmff", "mov,mp4,m4a,3gp,3g2,mj2"},
    FormatAlias{"ivf", "ivf"},
    FormatAlias{"obu", "obu"},
    FormatAlias{"y4m", "yuv4mpegpipe"},
    FormatAlias{"ebml", "matroska,webm"},
    FormatAlias{"mpegts", "mpegts"},
    FormatAlias{"mxf", "mxf"},
    // libavformat's own names for the Annex B cases (§8.7.2 rule 5: "h264"/"hevc" by sniffed codec, "av1" for
    // AV1 Annex B).
    FormatAlias{"h264", "h264"},
    FormatAlias{"hevc", "hevc"},
    FormatAlias{"av1", "av1"},
};

constexpr std::string_view kMovNames = "mov,mp4,m4a,3gp,3g2,mj2";
constexpr int64_t kIsobmffProbeSize = int64_t{5} << 20U;  // §8.7.2 rule 4

std::string av_error_text(int rc) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
  if (av_strerror(rc, buf.data(), buf.size()) < 0) {
    return "libavformat error " + std::to_string(rc);
  }
  return {buf.data()};
}

ovt_status map_av_error(int rc, bool io_failed) {
  if (io_failed || rc == AVERROR(EIO)) {
    return OVT_E_IO;
  }
  if (rc == AVERROR(ENOMEM)) {
    return OVT_E_NOMEM;
  }
  if (rc == AVERROR_DEMUXER_NOT_FOUND || rc == AVERROR_DECODER_NOT_FOUND || rc == AVERROR_PATCHWELCOME ||
      rc == AVERROR(ENOSYS)) {
    return OVT_E_UNSUPPORTED;
  }
  return OVT_E_CORRUPT;  // AVERROR_INVALIDDATA, AVERROR_EOF inside a header, and everything else
}

std::span<AVStream* const> streams_of(const AVFormatContext* fmt) {
  if (fmt->streams == nullptr) {
    return {};
  }
  return {fmt->streams, fmt->nb_streams};
}

std::span<const uint8_t> extradata_of(const AVCodecParameters* par) {
  if (par->extradata == nullptr || par->extradata_size <= 0) {
    return {};
  }
  return {par->extradata, static_cast<size_t>(par->extradata_size)};
}

// §8.7.6: codec_private, its fourcc and the NAL length size from codecpar->extradata.
void fill_codec_private(StreamInfo& info, const AVCodecParameters* par) {
  const std::span<const uint8_t> extra = extradata_of(par);
  switch (par->codec_id) {
    case AV_CODEC_ID_H264:
      // avcC: configurationVersion 1; lengthSizeMinusOne is the low 2 bits of byte 4.
      if (extra.size() >= 5 && extra[0] == 0x01) {
        info.codec_private_fourcc = "avcC";
        info.codec_private.assign(extra.begin(), extra.end());
        info.nal_length_size = static_cast<uint8_t>((extra[4] & 0x03U) + 1U);
        info.length_prefixed = true;
      } else {
        info.codec_private_fourcc = "annexb";
      }
      break;
    case AV_CODEC_ID_HEVC:
      // hvcC: configurationVersion 1, at least the 23-byte fixed part; lengthSizeMinusOne in byte 21.
      if (extra.size() >= 23 && extra[0] == 0x01) {
        info.codec_private_fourcc = "hvcC";
        info.codec_private.assign(extra.begin(), extra.end());
        info.nal_length_size = static_cast<uint8_t>((extra[21] & 0x03U) + 1U);
        info.length_prefixed = true;
      } else {
        info.codec_private_fourcc = "annexb";
      }
      break;
    case AV_CODEC_ID_AV1:
      // av1C: marker 1, version 1 → first byte 0x81.
      if (extra.size() >= 4 && extra[0] == 0x81) {
        info.codec_private_fourcc = "av1C";
        info.codec_private.assign(extra.begin(), extra.end());
      }
      break;
    case AV_CODEC_ID_VP9:
      // libavformat parses vpcC into codecpar fields and leaves extradata empty; the raw payload comes from
      // Role A's TrackInfo.codec_private (WI-1.232). Only ISOBMFF/Matroska carry a vpcC box.
      if (par->codec_tag == MKTAG('v', 'p', '0', '9')) {
        info.codec_private_fourcc = "vpcC";
      }
      break;
    default:
      break;
  }
}

StreamInfo stream_info(const AVStream* st) {
  const AVCodecParameters* par = st->codecpar;
  StreamInfo info;
  info.avstream_index = st->index;
  info.container_track_id = st->id != 0 ? st->id : -1;
  info.codec = codec_from_av(par->codec_id);
  info.media_type = par->codec_type;
  info.time_base = st->time_base;
  info.nb_frames = st->nb_frames > 0 ? st->nb_frames : 0;
  info.duration = st->duration;
  fill_codec_private(info, par);
  return info;
}

}  // namespace

ovt::fb::Codec codec_from_av(AVCodecID id) noexcept {
  switch (id) {
    case AV_CODEC_ID_H264:
      return ovt::fb::Codec::H264;
    case AV_CODEC_ID_HEVC:
      return ovt::fb::Codec::H265;
    case AV_CODEC_ID_AV1:
      return ovt::fb::Codec::AV1;
    case AV_CODEC_ID_VP9:
      return ovt::fb::Codec::VP9;
    case AV_CODEC_ID_VVC:
      return ovt::fb::Codec::H266;
    case AV_CODEC_ID_MPEG2VIDEO:
      return ovt::fb::Codec::MPEG2;
    default:
      return ovt::fb::Codec::Unknown;
  }
}

std::optional<std::string_view> libavformat_name(std::string_view hint_format) noexcept {
  if (hint_format.empty() || hint_format == "annexb") {
    return std::string_view{};
  }
  for (const auto& alias : kFormatAliases) {
    if (alias.ovt == hint_format) {
      return alias.lavf;
    }
  }
  return std::nullopt;
}

Demuxer::Demuxer() = default;

Demuxer::~Demuxer() {
  av_packet_free(&pkt_);
  // §8.7.2 / WI-1.230 pitfall: close the format context before the AVIOContext and its buffer are freed.
  avformat_close_input(&fmt_);
  avio_.reset();
}

ovt::Result<std::unique_ptr<Demuxer>> Demuxer::open(std::shared_ptr<ovt::io::RandomAccessFile> file,
                                                    std::string_view hint_format, const ovt::fb::ContainerTree* tree) {
  using R = ovt::Result<std::unique_ptr<Demuxer>>;
  if (!file) {
    return R::error(OVT_E_INVALID_ARG, "demux: no file");
  }
  if (hint_format == "raw") {
    // rawvideo needs -video_size/-pixel_format/-framerate from a RawVideoFormat (§8.7.2 rule 5), which open()
    // does not take yet; the raw YUV work item (WI-1.229) adds that path.
    return R::error(OVT_E_UNSUPPORTED, "demux: raw input needs a RawVideoFormat");
  }
  const std::optional<std::string_view> lavf_name = libavformat_name(hint_format);
  if (!lavf_name) {
    return R::error(OVT_E_INVALID_ARG, "demux: unknown hint_format '" + std::string(hint_format) + "'");
  }
  const AVInputFormat* ifmt = nullptr;
  if (!lavf_name->empty()) {
    ifmt = av_find_input_format(std::string(*lavf_name).c_str());
    if (ifmt == nullptr) {
      return R::error(OVT_E_UNSUPPORTED, "demux: libavformat has no '" + std::string(*lavf_name) + "' demuxer");
    }
  }

  if (file->size() == 0) {
    // Acceptance test 3 of WI-1.230: some demuxers (mpegts) accept an empty input as a file without streams.
    return R::error(OVT_E_CORRUPT, "demux: empty file");
  }

  try {
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): false positive (macOS libc++), every path destroys d
    std::unique_ptr<Demuxer> d(new Demuxer());
    d->file_ = std::move(file);
    d->tree_ = tree;
    d->avio_ = detail::AvioAdapter::create(d->file_);
    d->pkt_ = av_packet_alloc();
    AVFormatContext* fmt = avformat_alloc_context();
    if (!d->avio_ || d->pkt_ == nullptr || fmt == nullptr) {
      avformat_free_context(fmt);
      return R::error(OVT_E_NOMEM, "demux: out of memory");
    }
    fmt->pb = d->avio_->context();
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    // §8.7.2 rule 4. The options only apply to the demuxer that defines them; unknown ones stay in `opts`.
    AVDictionary* opts = nullptr;
    const std::string_view name = ifmt != nullptr ? std::string_view(ifmt->name) : std::string_view{};
    if (name == "mpegts") {
      av_dict_set(&opts, "fflags", "+genpts", 0);
    }
    if (name == kMovNames) {
      av_dict_set(&opts, "advanced_editlist", "0", 0);
      av_dict_set(&opts, "ignore_editlist", "0", 0);
      av_dict_set(&opts, "export_all", "1", 0);
      av_dict_set(&opts, "probesize", std::to_string(kIsobmffProbeSize).c_str(), 0);
    }
    // On failure avformat_open_input frees `fmt` (and sets it to null) but not the custom pb.
    const int rc = avformat_open_input(&fmt, nullptr, ifmt, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
      return R::error(map_av_error(rc, d->avio_->io_failed()), "demux: open failed: " + av_error_text(rc));
    }
    d->fmt_ = fmt;
    const int info_rc = avformat_find_stream_info(d->fmt_, nullptr);
    if (info_rc < 0) {
      return R::error(map_av_error(info_rc, d->avio_->io_failed()),
                      "demux: stream probe failed: " + av_error_text(info_rc));
    }
    if (d->fmt_->nb_streams == 0) {
      return R::error(OVT_E_CORRUPT, "demux: no streams found");
    }
    for (const AVStream* st : streams_of(d->fmt_)) {
      d->streams_.push_back(stream_info(st));
    }
    d->selected_.assign(d->streams_.size(), true);
    return R::ok(std::move(d));
  } catch (const std::bad_alloc&) {
    return R::error(OVT_E_NOMEM, "demux: out of memory");
  } catch (const std::exception& e) {
    return R::error(OVT_E_INTERNAL, std::string("demux: ") + e.what());
  }
}

void Demuxer::select_streams(std::span<const int> avstream_indices) {
  selected_.assign(streams_.size(), false);
  for (const int idx : avstream_indices) {
    if (idx >= 0 && static_cast<size_t>(idx) < selected_.size()) {
      selected_[static_cast<size_t>(idx)] = true;
    }
  }
  const std::span<AVStream* const> sts = streams_of(fmt_);
  for (size_t i = 0; i < sts.size() && i < selected_.size(); ++i) {
    sts[i]->discard = selected_[i] ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  }
}

ovt::Result<std::optional<Packet>> Demuxer::read_packet() {
  using R = ovt::Result<std::optional<Packet>>;
  av_packet_unref(pkt_);  // the previous Packet::data dies here
  for (;;) {
    const int rc = av_read_frame(fmt_, pkt_);
    if (rc == AVERROR_EOF && !avio_->io_failed()) {
      return R::ok(std::nullopt);
    }
    if (rc < 0) {
      return R::error(map_av_error(rc, avio_->io_failed()), "demux: read failed: " + av_error_text(rc));
    }
    const int idx = pkt_->stream_index;
    // Not every demuxer honours AVDISCARD_ALL, and a stream may appear after open (AVFMTCTX_NOHEADER).
    if (idx < 0 || static_cast<size_t>(idx) >= selected_.size() || !selected_[static_cast<size_t>(idx)]) {
      av_packet_unref(pkt_);
      continue;
    }
    Packet p;
    p.avstream_index = idx;
    p.pts = pkt_->pts;
    p.dts = pkt_->dts;
    p.duration = pkt_->duration;
    p.file_pos = pkt_->pos;
    p.keyframe = (static_cast<unsigned>(pkt_->flags) & static_cast<unsigned>(AV_PKT_FLAG_KEY)) != 0U;
    if (pkt_->data != nullptr && pkt_->size > 0) {
      p.data = std::span<const uint8_t>(pkt_->data, static_cast<size_t>(pkt_->size));
    }
    return R::ok(p);
  }
}

Status Demuxer::seek_to_time(int avstream_index, int64_t ts, bool backward) {
  if (avstream_index < 0 || static_cast<size_t>(avstream_index) >= streams_.size()) {
    return Status::error(OVT_E_INVALID_ARG, "demux: stream index out of range");
  }
  av_packet_unref(pkt_);
  // §8.7.5: backward → last sync point at or before ts; forward → first at or after ts.
  const int64_t min_ts = backward ? std::numeric_limits<int64_t>::min() : ts;
  const int64_t max_ts = backward ? ts : std::numeric_limits<int64_t>::max();
  const int rc = avformat_seek_file(fmt_, avstream_index, min_ts, ts, max_ts, backward ? AVSEEK_FLAG_BACKWARD : 0);
  if (rc < 0) {
    const ovt_status s = avio_->io_failed() ? OVT_E_IO : rc == AVERROR(ENOMEM) ? OVT_E_NOMEM : OVT_E_UNSUPPORTED;
    return Status::error(s, "demux: seek failed: " + av_error_text(rc));
  }
  return Status::ok();
}

}  // namespace ovt::demux
