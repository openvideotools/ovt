// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libavcodec h264 decoder driving of the FFmpeg analysis plugin (chapter 07 §7.6.3, WI-2.169).

#include "h264_analysis.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

// AV_CODEC_FLAG_COPY_OPAQUE arrived in FFmpeg 6.0 (libavcodec 60), the floor of §7.6.3; the pin is n7.1.1.
#if LIBAVCODEC_VERSION_MAJOR < 60
#error "ovt-plugin-ffmpeg needs FFmpeg >= 6.0 (AV_CODEC_FLAG_COPY_OPAQUE, chapter 07 §7.6.3)"
#endif

namespace ovt::ffmpeg {

namespace {

constexpr uint8_t kSeverityWarning = 1;  // ovt.fb.Severity
constexpr uint8_t kSeverityError = 2;
constexpr std::array<uint8_t, 4> kStartCode = {0, 0, 0, 1};

std::string av_error_text(int err) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
  av_strerror(err, buf.data(), buf.size());
  return {buf.data()};
}

void append_annexb(std::vector<uint8_t>* out, std::span<const uint8_t> nal) {
  out->insert(out->end(), kStartCode.begin(), kStartCode.end());
  out->insert(out->end(), nal.begin(), nal.end());
}

// The SPS and PPS of an AVCDecoderConfigurationRecord (ISO/IEC 14496-15 5.3.3.1) as Annex B units. The trailing
// chroma/bit-depth fields of High profiles are not needed. False when the record is truncated or malformed.
bool avcc_parameter_sets(std::span<const uint8_t> avcc, std::vector<uint8_t>* out) {
  if (avcc.size() < 7 || avcc[0] != 1) {
    return false;
  }
  size_t pos = 5;
  for (int array = 0; array < 2; ++array) {
    if (pos >= avcc.size()) {
      return false;
    }
    const unsigned count = array == 0 ? (avcc[pos] & 0x1FU) : avcc[pos];
    ++pos;
    for (unsigned i = 0; i < count; ++i) {
      if (pos + 2 > avcc.size()) {
        return false;
      }
      const size_t len = (size_t{avcc[pos]} << 8) | avcc[pos + 1];
      pos += 2;
      if (len == 0 || pos + len > avcc.size()) {
        return false;
      }
      append_annexb(out, avcc.subspan(pos, len));
      pos += len;
    }
  }
  return true;
}

uint8_t frame_type(AVPictureType t) {
  switch (t) {
    case AV_PICTURE_TYPE_I:
    case AV_PICTURE_TYPE_SI:
      return 0;
    case AV_PICTURE_TYPE_P:
    case AV_PICTURE_TYPE_SP:
      return 1;
    case AV_PICTURE_TYPE_B:
      return 2;
    default:
      return 7;
  }
}

bool is_key_frame(const AVFrame* f) {
#ifdef AV_FRAME_FLAG_KEY
  return (f->flags & AV_FRAME_FLAG_KEY) != 0;
#else
  return f->key_frame != 0;  // FFmpeg 6.0
#endif
}

// FrameInfo fields of an output frame. With apply_cropping = 0 the frame keeps the coded size and states the
// conformance window in crop_*.
Frame describe(const AVFrame* f) {
  Frame out;
  out.decode_index = static_cast<int32_t>(reinterpret_cast<intptr_t>(f->opaque));
  out.frame_type = frame_type(f->pict_type);
  out.is_rap = is_key_frame(f);
  out.coded_w = f->width;
  out.coded_h = f->height;
  out.crop_x = static_cast<int32_t>(f->crop_left);
  out.crop_y = static_cast<int32_t>(f->crop_top);
  out.crop_w = f->width - static_cast<int32_t>(f->crop_left + f->crop_right);
  out.crop_h = f->height - static_cast<int32_t>(f->crop_top + f->crop_bottom);
  if (const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format)); d != nullptr) {
    out.bit_depth_luma = static_cast<uint8_t>(d->comp[0].depth);
    if (d->nb_components < 3) {
      out.chroma_format = 0;
      out.bit_depth_chroma = out.bit_depth_luma;
    } else {
      out.bit_depth_chroma = static_cast<uint8_t>(d->comp[1].depth);
      out.chroma_format = d->log2_chroma_w == 0 ? 3 : (d->log2_chroma_h == 1 ? 1 : 2);
    }
  }
  return out;
}

// Copies the colour planes of a planar frame into tightly packed plugin buffers.
void copy_planes(const AVFrame* f, Frame* out) {
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
  if (d == nullptr || (d->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
    return;
  }
  const int planes = std::min(av_pix_fmt_count_planes(static_cast<AVPixelFormat>(f->format)), 3);
  for (int c = 0; c < planes; ++c) {
    Plane& p = out->planes[static_cast<size_t>(c)];
    const int depth = d->comp[c].depth;
    p.bit_depth = static_cast<uint8_t>(depth);
    p.sample_bytes = depth > 8 ? 2 : 1;
    const int shift_w = c == 0 ? 0 : d->log2_chroma_w;
    const int shift_h = c == 0 ? 0 : d->log2_chroma_h;
    p.width = static_cast<uint32_t>(AV_CEIL_RSHIFT(f->width, shift_w));
    p.height = static_cast<uint32_t>(AV_CEIL_RSHIFT(f->height, shift_h));
    const int row = static_cast<int>(p.width * p.sample_bytes);
    p.data.resize(static_cast<size_t>(row) * p.height);
    av_image_copy_plane(p.data.data(), row, f->data[c], f->linesize[c], row, static_cast<int>(p.height));
  }
}

}  // namespace

void AVFrameDeleter::operator()(AVFrame* f) const noexcept {
  av_frame_free(&f);
}

ovt_status H264Analysis::create(const uint8_t* codec_private, size_t len, int threads,
                                std::unique_ptr<H264Analysis>* out) {
  auto dec = std::make_unique<H264Analysis>();
  if (codec_private != nullptr && len > 0 &&
      !avcc_parameter_sets(std::span<const uint8_t>(codec_private, len), &dec->parameter_sets_)) {
    return OVT_E_CORRUPT;
  }
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (codec == nullptr) {
    return OVT_E_UNSUPPORTED;  // an FFmpeg build without the h264 decoder (pooled-codec-free variant, ADR-015)
  }
  dec->ctx_ = avcodec_alloc_context3(codec);
  dec->pkt_ = av_packet_alloc();
  dec->frame_ = av_frame_alloc();
  if (dec->ctx_ == nullptr || dec->pkt_ == nullptr || dec->frame_ == nullptr) {
    return OVT_E_NOMEM;
  }
  AVCodecContext* ctx = dec->ctx_;
  ctx->export_side_data = AV_CODEC_EXPORT_DATA_MVS | AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS;
  ctx->thread_count = std::max(threads, 1);
  ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
  ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
  ctx->apply_cropping = 0;  // planes in coded-picture coordinates, like the grid; the crop rect goes to FrameInfo
  // No extradata and no hwaccel, ever (§7.6.3).
  const int err = avcodec_open2(ctx, codec, nullptr);
  if (err == AVERROR(ENOMEM)) {
    return OVT_E_NOMEM;
  }
  if (err < 0) {
    return OVT_E_PLUGIN;
  }
  *out = std::move(dec);
  return OVT_OK;
}

H264Analysis::~H264Analysis() {
  av_frame_free(&frame_);
  av_packet_free(&pkt_);
  avcodec_free_context(&ctx_);
}

void H264Analysis::add_diag(uint8_t severity, std::string code, std::string message) {
  pending_diags_.push_back(Diag{severity, std::move(code), std::move(message)});
}

ovt_status H264Analysis::push(const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts) {
  if (unit == nullptr || len == 0 || flushed_) {
    return OVT_E_INVALID_ARG;
  }
  ovt_status st = OVT_OK;
  if (has_au_ && decode_index_hint != au_hint_) {
    st = send_access_unit();
  }
  if (!has_au_) {
    au_.clear();
    if (!parameter_sets_.empty()) {
      au_ = std::move(parameter_sets_);
      parameter_sets_.clear();
    }
    has_au_ = true;
    au_hint_ = decode_index_hint;
    pts_[decode_index_hint] = pts;
  }
  append_annexb(&au_, std::span<const uint8_t>(unit, len));
  return st;
}

ovt_status H264Analysis::flush() {
  if (flushed_) {
    return OVT_OK;
  }
  ovt_status st = OVT_OK;
  if (has_au_) {
    st = send_access_unit();
  }
  flushed_ = true;
  const ovt_status drained = send(nullptr);  // enter draining mode: delayed frames come out until AVERROR_EOF
  return st != OVT_OK ? st : drained;
}

ovt_status H264Analysis::send_access_unit() {
  has_au_ = false;
  av_packet_unref(pkt_);
  if (av_new_packet(pkt_, static_cast<int>(au_.size())) < 0) {
    throw std::bad_alloc();
  }
  std::memcpy(pkt_->data, au_.data(), au_.size());
  pkt_->pts = au_hint_;
  // The decode index travels in opaque (AV_CODEC_FLAG_COPY_OPAQUE); libavcodec copies it to the frame that this
  // packet starts, also across frame threads.
  pkt_->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(au_hint_));  // NOLINT(performance-no-int-to-ptr)
  const ovt_status st = send(pkt_);
  av_packet_unref(pkt_);
  return st;
}

ovt_status H264Analysis::send(const AVPacket* pkt) {
  ovt_status st = OVT_OK;
  for (;;) {
    const int err = avcodec_send_packet(ctx_, pkt);
    if (err == AVERROR(EAGAIN)) {
      // The decoder wants its output read first; receive_all() empties it, so the next send succeeds.
      st = receive_all();
      continue;
    }
    if (err == AVERROR(ENOMEM)) {
      throw std::bad_alloc();
    }
    if (err < 0 && err != AVERROR_EOF) {
      // SPEC-ASSUMPTION(#261): no chapter 07 §7.9.5 code exists for a libavcodec decode error; the message names it.
      add_diag(kSeverityError, "",
               "libavcodec rejected the access unit of decode index " + std::to_string(au_hint_) + ": " +
                   av_error_text(err));
      st = OVT_E_CORRUPT;
    }
    break;
  }
  const ovt_status received = receive_all();
  return st != OVT_OK ? st : received;
}

ovt_status H264Analysis::receive_all() {
  for (;;) {
    const int err = avcodec_receive_frame(ctx_, frame_);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
      return OVT_OK;
    }
    if (err == AVERROR(ENOMEM)) {
      throw std::bad_alloc();
    }
    if (err < 0) {
      add_diag(kSeverityError, "", "libavcodec decode error: " + av_error_text(err));
      return OVT_E_CORRUPT;
    }
    Pending p;
    p.info = describe(frame_);
    if (const auto it = pts_.find(p.info.decode_index); it != pts_.end()) {
      p.info.pts = it->second;
      pts_.erase(it);
    }
    if (frame_->decode_error_flags != 0) {
      add_diag(kSeverityWarning, "",
               "libavcodec concealed errors in this frame (decode_error_flags " +
                   std::to_string(frame_->decode_error_flags) + ")");
    }
    p.info.diagnostics = std::move(pending_diags_);
    pending_diags_.clear();
    p.frame.reset(av_frame_alloc());
    if (p.frame == nullptr) {
      throw std::bad_alloc();
    }
    av_frame_move_ref(p.frame.get(), frame_);
    frames_.push_back(std::move(p));
  }
}

bool H264Analysis::pop(Frame* out, bool want_planes) {
  if (frames_.empty()) {
    return false;
  }
  Pending p = std::move(frames_.front());
  frames_.pop_front();
  if (want_planes) {
    copy_planes(p.frame.get(), &p.info);
  }
  *out = std::move(p.info);
  return true;
}

}  // namespace ovt::ffmpeg
