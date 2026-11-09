// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// libaom decoder driving of the AV1 analysis plugin (chapter 07 §7.6.1 "Decoder driving", WI-2.155).

#include "decoder.hpp"

#include <aom/aom_decoder.h>
#include <aom/aom_image.h>
#include <aom/aomdx.h>

// Semi-public: not installed by libaom, included from the pinned source tree (tp::aom_inspection).
#include "av1/decoder/inspection.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ovt::aom {

namespace {

constexpr uint8_t kSeverityError = 2;  // ovt.fb.Severity

// OBU types (AV1 §6.2.2).
constexpr unsigned kObuTemporalDelimiter = 2;
constexpr unsigned kObuFrameHeader = 3;
constexpr unsigned kObuFrame = 6;

constexpr const char* kNoInspection = "libaom built without CONFIG_INSPECTION";

// ovt.fb.FrameType of libaom's FRAME_TYPE (KEY_FRAME 0, INTER_FRAME 1, INTRA_ONLY_FRAME 2, S_FRAME 3).
uint8_t fb_frame_type(int t) {
  constexpr uint8_t kKey = 3;
  constexpr uint8_t kUnknown = 7;
  return t >= 0 && t <= 3 ? static_cast<uint8_t>(kKey + t) : kUnknown;
}

// Checks one OBU (AV1 §5.3.1): forbidden bit clear, obu_has_size_field set, and the header, leb128 obu_size and
// payload exactly fill @p obu. Returns the obu_type, or an error text in @p why.
bool parse_obu(std::span<const uint8_t> obu, unsigned* type, std::string* why) {
  if (obu.empty()) {
    *why = "empty OBU";
    return false;
  }
  const uint8_t h = obu[0];
  if ((h & 0x80U) != 0) {
    *why = "obu_forbidden_bit is set";
    return false;
  }
  if ((h & 0x02U) == 0) {
    *why = "OBU without obu_has_size_field";
    return false;
  }
  *type = (h >> 3U) & 0x0FU;
  size_t pos = (h & 0x04U) != 0 ? 2 : 1;  // obu_extension_flag adds one header byte
  uint64_t size = 0;
  for (int i = 0;; ++i) {  // leb128(), AV1 §4.10.5: at most 8 bytes
    if (i == 8 || pos >= obu.size()) {
      *why = "truncated obu_size";
      return false;
    }
    const uint8_t b = obu[pos++];
    size |= uint64_t{b & 0x7FU} << (7U * static_cast<unsigned>(i));
    if ((b & 0x80U) == 0) {
      break;
    }
  }
  if (size != obu.size() - pos) {
    *why = "obu_size does not match the unit length";
    return false;
  }
  return true;
}

uint8_t chroma_format_of(aom_img_fmt_t fmt, bool monochrome) {
  if (monochrome) {
    return 0;
  }
  switch (static_cast<unsigned>(fmt) & ~static_cast<unsigned>(AOM_IMG_FMT_HIGHBITDEPTH)) {
    case AOM_IMG_FMT_I422:
      return 2;
    case AOM_IMG_FMT_I444:
      return 3;
    default:
      return 1;
  }
}

// Copies the visible part of plane @p p of @p img (d_w × d_h luma; chroma rounded up by the subsampling).
Plane copy_plane(const aom_image_t& img, int p) {
  Plane out;
  out.sample_bytes = (static_cast<unsigned>(img.fmt) & AOM_IMG_FMT_HIGHBITDEPTH) != 0 ? 2 : 1;
  out.bit_depth = static_cast<uint8_t>(img.bit_depth);
  const unsigned sx = p == 0 ? 0 : img.x_chroma_shift;
  const unsigned sy = p == 0 ? 0 : img.y_chroma_shift;
  out.width = (img.d_w + sx) >> sx;
  out.height = (img.d_h + sy) >> sy;
  const size_t row = size_t{out.width} * out.sample_bytes;
  const auto stride = static_cast<size_t>(img.stride[p]);
  out.data.resize(row * out.height);
  const std::span<const uint8_t> src(img.planes[p], stride * (out.height - 1) + row);
  for (uint32_t y = 0; y < out.height; ++y) {
    std::memcpy(std::span<uint8_t>(out.data).subspan(row * y).data(), src.subspan(stride * y).data(), row);
  }
  return out;
}

}  // namespace

struct Decoder::Impl {
  aom_codec_ctx_t ctx{};
  bool open = false;
  insp_frame_data fd{};
  bool fd_init = false;
  // Filled by the inspection callback, read after aom_codec_decode() returns.
  bool fired = false;
  int frame_type = -1;
  bool show_frame = false;
  int base_qindex = 0;
  bool monochrome = false;  // sequence-level, from the last output image

  Impl() = default;
  ~Impl() {
    if (open) {
      aom_codec_destroy(&ctx);
    }
    if (fd_init) {
      ifd_clear(&fd);
    }
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  // The Frame of the frame the last aom_codec_decode() call handled with the callback: frame-level fields from
  // the callback, sizes and format from codec queries (after the call, never from inside the callback), and the
  // planes of the image it showed, which are valid only until the next aom_codec_decode().
  Frame take_frame(int32_t decode_index, int64_t pts) {
    Frame f;
    f.decode_index = decode_index;
    f.pts = pts;
    f.frame_type = fb_frame_type(frame_type);
    f.shown = show_frame;
    f.qp_base = static_cast<int16_t>(base_qindex);
    std::array<int, 2> size{};
    if (aom_codec_control(&ctx, AV1D_GET_FRAME_SIZE, size.data()) == AOM_CODEC_OK) {
      f.coded_w = size[0];
      f.coded_h = size[1];
    }
    std::array<int, 2> render{};
    if (aom_codec_control(&ctx, AV1D_GET_DISPLAY_SIZE, render.data()) == AOM_CODEC_OK) {
      f.render_w = render[0];
      f.render_h = render[1];
    }
    unsigned bit_depth = 8;
    if (aom_codec_control(&ctx, AV1D_GET_BIT_DEPTH, &bit_depth) == AOM_CODEC_OK) {
      f.bit_depth = static_cast<uint8_t>(bit_depth);
    }
    aom_img_fmt_t fmt = AOM_IMG_FMT_I420;
    const bool have_fmt = aom_codec_control(&ctx, AV1D_GET_IMG_FORMAT, &fmt) == AOM_CODEC_OK;
    aom_codec_iter_t iter = nullptr;
    while (const aom_image_t* img = aom_codec_get_frame(&ctx, &iter)) {
      if (!f.shown || !f.planes[0].data.empty()) {
        continue;
      }
      monochrome = img->monochrome != 0;
      for (int p = 0; p < (monochrome ? 1 : 3); ++p) {
        f.planes.at(static_cast<size_t>(p)) = copy_plane(*img, p);
      }
    }
    f.chroma_format = have_fmt ? chroma_format_of(fmt, monochrome) : 1;
    return f;
  }

  void discard_images() {
    aom_codec_iter_t iter = nullptr;
    while (aom_codec_get_frame(&ctx, &iter) != nullptr) {
    }
  }

  // The inspection callback (aom_inspect_cb). It runs inside aom_codec_decode(), possibly on a libaom worker
  // thread with row-MT, after the frame's last tile group: it only copies, and never calls back into the codec
  // context (chapter 07 WI-2.155 notes). ifd_inspect() reads the decoder's own state, which is allowed.
  static void on_frame(void* decoder, void* data) {
    auto* self = static_cast<Impl*>(data);
    ifd_inspect(&self->fd, decoder, /*skip_not_transform=*/0);
    self->frame_type = self->fd.frame_type;
    self->show_frame = self->fd.show_frame != 0;
    self->base_qindex = self->fd.base_qindex;
    self->fired = true;
  }
};

Decoder::~Decoder() = default;

ovt_status Decoder::create(int threads, std::unique_ptr<Decoder>* out, std::string* error) {
  auto dec = std::make_unique<Decoder>();
  dec->impl_ = std::make_unique<Impl>();
  Impl& im = *dec->impl_;
  aom_codec_dec_cfg_t cfg{};
  cfg.threads = static_cast<unsigned>(std::max(1, threads));
  cfg.allow_lowbitdepth = 1;  // 8-bit streams give 8-bit planes, as aomdec and inspect do
  if (aom_codec_dec_init(&im.ctx, aom_codec_av1_dx(), &cfg, 0) != AOM_CODEC_OK) {
    *error = std::string("aom_codec_dec_init failed: ") + aom_codec_error(&im.ctx);
    return OVT_E_PLUGIN;
  }
  im.open = true;
  // The host delivers Section 5 OBUs; operating point 0, one output layer; reconstruction before film grain
  // synthesis (chapter 06 §6.6.1 rule 5). AV1_SET_SKIP_LOOP_FILTER is never set in analysis mode.
  if (aom_codec_control(&im.ctx, AV1D_SET_IS_ANNEXB, 0U) != AOM_CODEC_OK ||
      aom_codec_control(&im.ctx, AV1D_SET_OUTPUT_ALL_LAYERS, 0) != AOM_CODEC_OK ||
      aom_codec_control(&im.ctx, AV1D_SET_OPERATING_POINT, 0) != AOM_CODEC_OK ||
      aom_codec_control(&im.ctx, AV1D_SET_ROW_MT, 1U) != AOM_CODEC_OK ||
      aom_codec_control(&im.ctx, AV1D_SET_SKIP_FILM_GRAIN, 1) != AOM_CODEC_OK) {
    *error = std::string("libaom decoder control failed: ") + aom_codec_error(&im.ctx);
    return OVT_E_PLUGIN;
  }
  aom_inspect_init ii{};
  ii.inspect_cb = &Impl::on_frame;
  ii.inspect_ctx = &im;
  const aom_codec_err_t err = aom_codec_control(&im.ctx, AV1_SET_INSPECTION_CALLBACK, &ii);
  if (err == AOM_CODEC_INCAPABLE) {
    *error = kNoInspection;
    return OVT_E_PLUGIN;
  }
  if (err != AOM_CODEC_OK) {
    *error = std::string("AV1_SET_INSPECTION_CALLBACK failed: ") + aom_codec_error(&im.ctx);
    return OVT_E_PLUGIN;
  }
  // ifd_inspect() resizes the MI grid to each frame; start with a non-empty one (libaom aborts on a failed
  // allocation, and aom_malloc(0) may return NULL).
  ifd_init(&im.fd, 8, 8);
  im.fd_init = true;
  *out = std::move(dec);
  return OVT_OK;
}

void Decoder::add_diag(uint8_t severity, std::string code, std::string message) {
  pending_diags_.push_back(Diag{severity, std::move(code), std::move(message)});
}

ovt_status Decoder::push(const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts) {
  if (unit == nullptr || len == 0) {
    return OVT_E_INVALID_ARG;
  }
  const std::span<const uint8_t> obu(unit, len);
  unsigned type = 0;
  std::string why;
  if (!parse_obu(obu, &type, &why)) {
    // SPEC-ASSUMPTION(#261): decode errors become Diagnostics with an empty code, as in the other plugins.
    add_diag(kSeverityError, "", "OBU at decode index " + std::to_string(decode_index_hint) + " rejected: " + why);
    return OVT_E_CORRUPT;
  }
  ovt_status st = OVT_OK;
  if (type == kObuTemporalDelimiter) {
    if (!tu_frames_.empty()) {
      st = decode_temporal_unit();
    }
    // Units before the first frame OBU (sequence header, metadata) stay and travel with the next frame.
    if (st == OVT_OK && expected_next_ >= 0 && decode_index_hint != expected_next_) {
      last_error_ = "frame count mismatch at decode index " + std::to_string(decode_index_hint);
      return OVT_E_PLUGIN;
    }
  }
  if (type == kObuFrame || type == kObuFrameHeader) {
    if (tu_frames_.empty()) {
      tu_first_hint_ = decode_index_hint;
    }
    tu_frames_.push_back(FrameObu{tu_.size(), pts});
  }
  tu_.insert(tu_.end(), obu.begin(), obu.end());
  return st;
}

// The inspect.c loop of §7.6.1 over the collected temporal unit. Each aom_codec_decode() call handles exactly
// one frame: a frame with tile data fires the callback (hidden frames too); a show_existing_frame header fires
// none. A frame's position is the number of frame OBUs consumed before it, so the decode index does not depend
// on Av1DecodeReturn.show_existing, which keeps its old value when a call consumes only trailing non-frame OBUs.
ovt_status Decoder::decode_temporal_unit() {
  Impl& im = *impl_;
  const std::span<const uint8_t> tu(tu_);
  size_t off = 0;
  size_t next_frame_obu = 0;  // index into tu_frames_
  int32_t pos = 0;
  ovt_status st = OVT_OK;
  while (off < tu.size()) {
    const std::span<const uint8_t> rest = tu.subspan(off);
    im.fired = false;
    Av1DecodeReturn ret{};  // user_priv MUST point here on every data call: NULL disables inspection
    if (aom_codec_decode(&im.ctx, rest.data(), rest.size(), &ret) != AOM_CODEC_OK) {
      const char* detail = aom_codec_error_detail(&im.ctx);
      add_diag(kSeverityError, "",
               "libaom: " + std::string(aom_codec_error(&im.ctx)) +
                   (detail != nullptr ? std::string(": ") + detail : "") + " (temporal unit starting at decode index " +
                   std::to_string(tu_first_hint_) + ")");
      st = OVT_E_CORRUPT;
      break;
    }
    const auto consumed = static_cast<size_t>(ret.buf - rest.data());
    if (ret.buf == nullptr || consumed == 0 || consumed > rest.size()) {
      // libaom stopped without an error. Only trailing non-frame OBUs left: nothing is lost. Frame OBUs left:
      // say so here, rather than leave the next temporal unit to report a bare frame-count mismatch.
      if (next_frame_obu < tu_frames_.size()) {
        add_diag(kSeverityError, "",
                 "libaom stopped inside the temporal unit starting at decode index " + std::to_string(tu_first_hint_) +
                     "; " + std::to_string(tu_frames_.size() - next_frame_obu) + " frame(s) not decoded");
        st = OVT_E_CORRUPT;
      }
      break;
    }
    off += consumed;
    int32_t n = 0;  // frame OBUs consumed by this call
    while (next_frame_obu < tu_frames_.size() && tu_frames_[next_frame_obu].offset < off) {
      ++next_frame_obu;
      ++n;
    }
    if (im.fired) {
      n = std::max(n, 1);
      Frame f = im.take_frame(tu_first_hint_ + pos + n - 1, tu_frames_[std::max<size_t>(next_frame_obu, 1) - 1].pts);
      f.diagnostics = std::move(pending_diags_);
      pending_diags_.clear();
      frames_.push_back(std::move(f));
    } else {
      im.discard_images();  // show_existing_frame: the image belongs to the frame decoded earlier
    }
    pos += n;
  }
  expected_next_ = st == OVT_OK ? tu_first_hint_ + pos : -1;
  tu_.clear();
  tu_frames_.clear();
  tu_first_hint_ = -1;
  return st;
}

ovt_status Decoder::flush() {
  ovt_status st = OVT_OK;
  if (!tu_frames_.empty()) {
    st = decode_temporal_unit();
  }
  tu_.clear();
  // Normal-path flush (user_priv NULL is correct here: there is no data); inspection mode has no frame delay.
  aom_codec_decode(&impl_->ctx, nullptr, 0, nullptr);
  impl_->discard_images();
  return st;
}

bool Decoder::pop(Frame* out) {
  if (frames_.empty()) {
    return false;
  }
  *out = std::move(frames_.front());
  frames_.pop_front();
  return true;
}

}  // namespace ovt::aom
