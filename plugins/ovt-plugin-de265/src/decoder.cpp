// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// libde265 driving for ovt-plugin-de265 (chapter 07 §7.6.2 "Decoder driving", WI-2.163). Verified against the
// pinned libde265 1.0.15: de265.h, decctx.cc decoder_context::decode()/decode_some(), image.cc alloc_image().

#include "decoder.hpp"

#include "planes.hpp"

#include <libde265/de265.h>
#ifdef OVT_DE265_INTERNALS
#include <libde265/de265_internals.h>
#endif

#include <algorithm>
#include <climits>
#include <new>
#include <span>
#include <utility>

namespace ovt::de265 {
namespace {

// ovt.fb.Severity values (core/schema/ovt_model.fbs).
constexpr uint8_t kInfo = 0;
constexpr uint8_t kWarning = 1;
constexpr uint8_t kError = 2;

// Outstanding-frame limit (WI-2.163 step 4): decoding pauses while this many finished frames wait for pull(), so a
// host that pushes a long run of units without pulling cannot grow the plugin's memory without bound. libde265's
// own pictures are released at once and never count against its DPB.
constexpr std::size_t kMaxPendingFrames = 16;
// Diagnostics kept for the next frame; more are counted in one summary line.
constexpr std::size_t kMaxPendingDiags = 64;
// Safety net against a decode loop that makes no progress on corrupt input.
constexpr int kMaxDecodeCalls = 1 << 20;
constexpr int kMaxConsecutiveErrors = 8;
// libde265 1.0.15 MAX_THREADS; more makes de265_start_worker_threads warn and clamp.
constexpr int kMaxThreads = 32;
// pts_ entries for pictures libde265 never outputs (pic_output_flag = 0, skipped RASL) are dropped beyond this.
constexpr std::size_t kMaxPtsEntries = 256;

constexpr int kNalVps = 32;
constexpr int kNalSps = 33;
constexpr int kNalVclMax = 31;

constexpr const char* kUseHm = "use ovt-plugin-hm";

enum class Support : uint8_t { Ok, Unsupported };

// The first @p n RBSP bytes of a NAL unit (emulation-prevention bytes 0x000003 removed).
std::vector<uint8_t> rbsp_prefix(std::span<const uint8_t> unit, std::size_t n) {
  std::vector<uint8_t> out;
  out.reserve(n);
  int zeros = 0;
  for (std::size_t i = 0; i < unit.size() && out.size() < n; ++i) {
    if (zeros >= 2 && unit[i] == 0x03) {
      zeros = 0;
      continue;
    }
    zeros = unit[i] == 0 ? zeros + 1 : 0;
    out.push_back(unit[i]);
  }
  return out;
}

// Chapter 07 §7.6.2: libde265 is relied on for Main, Main 10 and Main Still Picture only. An SPS (or VPS) whose
// profile_tier_level names another profile (RExt 4, SCC 9, multi-layer 6..8, ...) and any unit of an enhancement
// layer (nuh_layer_id > 0) are refused so that the host can retry with ovt-plugin-hm.
Support check_unit(std::span<const uint8_t> unit) {
  if (unit.size() < 2) {
    return Support::Ok;  // libde265 reports the broken header
  }
  const int type = (unit[0] >> 1) & 0x3F;
  const int layer_id = ((unit[0] & 0x01) << 5) | (unit[1] >> 3);
  if (layer_id != 0) {
    return Support::Unsupported;
  }
  // general_profile_space u(2), general_tier_flag u(1), general_profile_idc u(5), then 32 compatibility flags:
  // RBSP byte 3 of an SPS (after sps_video_parameter_set_id .. sps_temporal_id_nesting_flag), byte 6 of a VPS.
  std::size_t ptl = 0;
  if (type == kNalSps) {
    ptl = 3;
  } else if (type == kNalVps) {
    ptl = 6;
  } else {
    return Support::Ok;
  }
  const std::vector<uint8_t> rbsp = rbsp_prefix(unit, ptl + 5);
  if (rbsp.size() < ptl + 5) {
    return Support::Ok;  // too short to tell; libde265 rejects the parameter set itself
  }
  const int profile_idc = rbsp[ptl] & 0x1F;
  if (profile_idc >= 1 && profile_idc <= 3) {
    return Support::Ok;
  }
  if (profile_idc != 0) {
    return Support::Unsupported;
  }
  // profile_idc 0: decide by general_profile_compatibility_flag[j] (bit 7 of byte ptl+1 is flag 0).
  const uint32_t compat = (uint32_t{rbsp[ptl + 1]} << 24) | (uint32_t{rbsp[ptl + 2]} << 16) |
                          (uint32_t{rbsp[ptl + 3]} << 8) | uint32_t{rbsp[ptl + 4]};
  const auto flag = [compat](int j) { return ((compat >> (31 - j)) & 1U) != 0; };
  if (flag(1) || flag(2) || flag(3)) {
    return Support::Ok;
  }
  for (int j = 4; j < 32; ++j) {
    if (flag(j)) {
      return Support::Unsupported;
    }
  }
  return Support::Ok;
}

de265_decoder_context* as_ctx(void* ctx) {
  return static_cast<de265_decoder_context*>(ctx);
}

// Picture buffers. The public API reports only the cropped picture (de265_get_image_width, de265_get_image_plane),
// but planes and hashes cover the whole decoded picture (chapter 06 V-023, H.265 D.3.19). The plugin therefore
// allocates libde265's picture buffers itself and hangs a Buffer record on them as plane user data; libde265 moves
// that user data together with the pixels when SAO exchanges a picture's buffers with its SAO output
// (image.cc exchange_pixel_data_with), so de265_get_image_plane_user_data() of an output picture always names the
// buffer it really holds.
struct Buffer {
  std::array<uint8_t*, 3> planes{};
  int width = 0;
  int height = 0;
  int crop_left = 0;
  int crop_top = 0;
  int visible_width = 0;
  int visible_height = 0;
};

constexpr std::size_t kPlaneAlignment = 16;  // libde265's STANDARD_ALIGNMENT; the SSE kernels need it
constexpr std::size_t kPlanePadding = 16;    // libde265's MEMORY_PADDING: SIMD reads past the last row

void free_buffer(Buffer* buf) {
  for (uint8_t* p : buf->planes) {
    if (p != nullptr) {
      ::operator delete(p, std::align_val_t{kPlaneAlignment});
    }
  }
  delete buf;  // NOLINT(cppcoreguidelines-owning-memory): owned through libde265's plane user data
}

// Mirrors libde265's de265_image_get_buffer() (image.cc): rows padded to spec->alignment samples.
int get_buffer(de265_decoder_context* /*ctx*/, de265_image_spec* spec, de265_image* img, void* /*userdata*/) {
  auto* buf = new (std::nothrow) Buffer();  // NOLINT(cppcoreguidelines-owning-memory)
  if (buf == nullptr) {
    return 0;
  }
  buf->width = spec->width;
  buf->height = spec->height;
  buf->crop_left = spec->crop_left;
  buf->crop_top = spec->crop_top;
  buf->visible_width = spec->visible_width;
  buf->visible_height = spec->visible_height;
  const de265_chroma chroma = de265_get_chroma_format(img);
  const int sub_w = (chroma == de265_chroma_420 || chroma == de265_chroma_422) ? 2 : 1;
  const int sub_h = chroma == de265_chroma_420 ? 2 : 1;
  const int planes = chroma == de265_chroma_mono ? 1 : 3;
  const int align = std::max(spec->alignment, 1);
  std::array<int, 3> strides{};
  for (int c = 0; c < planes; ++c) {
    const int w = c == 0 ? spec->width : spec->width / sub_w;
    const int h = c == 0 ? spec->height : spec->height / sub_h;
    const int bytes = (de265_get_bits_per_pixel(img, c) + 7) / 8;
    strides[static_cast<std::size_t>(c)] = (w + align - 1) / align * align * bytes;
    const std::size_t size =
        (static_cast<std::size_t>(strides[static_cast<std::size_t>(c)]) * static_cast<std::size_t>(h)) + kPlanePadding;
    buf->planes[static_cast<std::size_t>(c)] =
        static_cast<uint8_t*>(::operator new(size, std::align_val_t{kPlaneAlignment}, std::nothrow));
    if (buf->planes[static_cast<std::size_t>(c)] == nullptr) {
      free_buffer(buf);
      return 0;
    }
  }
  for (int c = 0; c < 3; ++c) {
    de265_set_image_plane(img, c, buf->planes[static_cast<std::size_t>(c)], strides[static_cast<std::size_t>(c)],
                          c < planes ? buf : nullptr);
  }
  return 1;
}

void release_buffer(de265_decoder_context* /*ctx*/, de265_image* img, void* /*userdata*/) {
  if (auto* buf = static_cast<Buffer*>(de265_get_image_plane_user_data(img, 0)); buf != nullptr) {
    free_buffer(buf);
  }
}

de265_image_allocation g_allocation = {get_buffer, release_buffer};  // libde265 copies it

}  // namespace

ovt_status Decoder::create(const uint8_t* codec_private, size_t len, int threads, std::unique_ptr<Decoder>* out) {
  auto dec = std::make_unique<Decoder>();  // std::bad_alloc becomes OVT_E_NOMEM in plugin.cpp
  de265_decoder_context* ctx = de265_new_decoder();
  if (ctx == nullptr) {
    return OVT_E_NOMEM;
  }
  dec->ctx_ = ctx;
  // The hash check is OVT's verify job (chapter 07 §7.6.2); DISABLE_DEBLOCKING/SAO are never set (bit-exactness).
  de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH, 0);
  de265_set_parameter_int(ctx, DE265_DECODER_PARAM_ACCELERATION_CODE, de265_acceleration_AUTO);
  de265_set_image_allocation_functions(ctx, &g_allocation, nullptr);
  if (threads > 0 && de265_isOK(de265_start_worker_threads(ctx, std::min(threads, kMaxThreads))) == 0) {
    return OVT_E_PLUGIN;
  }

  if (codec_private != nullptr && len > 0) {
    const ovt_status st = dec->push_hvcc(std::span<const uint8_t>(codec_private, len));
    if (st != OVT_OK) {
      return st;
    }
  }
  *out = std::move(dec);
  return OVT_OK;
}

// hvcC (ISO/IEC 14496-15 §8.3.3.1): 22 bytes of configuration, numOfArrays, then per array one byte
// (array_completeness, NAL_unit_type), numNalus u(16) and numNalus × (nalUnitLength u(16), NAL unit).
ovt_status Decoder::push_hvcc(std::span<const uint8_t> hvcc) {
  if (hvcc.size() < 23) {
    return OVT_E_CORRUPT;
  }
  std::size_t pos = 22;
  const int arrays = hvcc[pos++];
  for (int a = 0; a < arrays; ++a) {
    if (hvcc.size() - pos < 3) {
      return OVT_E_CORRUPT;
    }
    const std::size_t count = (std::size_t{hvcc[pos + 1]} << 8) | hvcc[pos + 2];
    pos += 3;
    for (std::size_t k = 0; k < count; ++k) {
      if (hvcc.size() - pos < 2) {
        return OVT_E_CORRUPT;
      }
      const std::size_t size = (std::size_t{hvcc[pos]} << 8) | hvcc[pos + 1];
      pos += 2;
      if (hvcc.size() - pos < size) {
        return OVT_E_CORRUPT;
      }
      const std::span<const uint8_t> nal = hvcc.subspan(pos, size);
      const ovt_status st = push(nal.data(), nal.size(), -1, 0);
      if (st != OVT_OK) {
        return st;
      }
      pos += size;
    }
  }
  return OVT_OK;
}

Decoder::~Decoder() {
  if (ctx_ != nullptr) {
    de265_free_decoder(as_ctx(ctx_));  // releases every picture buffer through release_buffer()
  }
}

void Decoder::add_diag(uint8_t severity, std::string code, std::string message) {
  if (pending_diags_.size() < kMaxPendingDiags) {
    pending_diags_.push_back(Diag{severity, std::move(code), std::move(message)});
  } else if (pending_diags_.size() == kMaxPendingDiags) {
    pending_diags_.push_back(Diag{kInfo, "", "further libde265 diagnostics dropped"});
  }
}

ovt_status Decoder::push(const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts) {
  if (flushed_ || unit == nullptr || len == 0 || len > static_cast<std::size_t>(INT_MAX)) {
    return OVT_E_INVALID_ARG;
  }
  const std::span<const uint8_t> bytes(unit, len);
  if (check_unit(bytes) == Support::Unsupported) {
    // SPEC-ASSUMPTION(#249): no frame will come for this unit, so its Diagnostic travels in a diagnostics-only
    // FrameAnalysis that the next pull() returns.
    Frame f;
    f.picture = false;
    f.decode_index = decode_index_hint;
    f.pts = pts;
    f.diagnostics = std::move(pending_diags_);
    pending_diags_.clear();
    f.diagnostics.push_back(Diag{kInfo, "DEC-H265-0010", kUseHm});
    frames_.push_back(std::move(f));
    return OVT_E_UNSUPPORTED;
  }
  return push_nal(bytes, decode_index_hint, pts);
}

ovt_status Decoder::push_nal(std::span<const uint8_t> unit, int32_t decode_index_hint, int64_t pts) {
  const int type = (unit[0] >> 1) & 0x3F;
  if (decode_index_hint >= 0 && type <= kNalVclMax) {
    pts_[decode_index_hint] = pts;
    while (pts_.size() > kMaxPtsEntries) {
      pts_.erase(pts_.begin());
    }
  }
  // libde265 copies pts and user_data from the first slice NAL into the picture; both carry the decode index
  // (chapter 07 §7.6.2), because pictures come out in output order.
  const de265_error err = de265_push_NAL(
      as_ctx(ctx_), unit.data(), static_cast<int>(unit.size()), decode_index_hint,
      reinterpret_cast<void*>(static_cast<intptr_t>(decode_index_hint)));  // NOLINT(performance-no-int-to-ptr)
  if (de265_isOK(err) == 0) {
    return err == DE265_ERROR_OUT_OF_MEMORY ? OVT_E_NOMEM : OVT_E_CORRUPT;
  }
  return decode_loop();
}

ovt_status Decoder::flush() {
  if (!flushed_) {
    flushed_ = true;
    de265_flush_data(as_ctx(ctx_));
  }
  return decode_loop();
}

// SPEC-ASSUMPTION(#449): the mask of any pull(), even one that returns no frame, is a standing request for the
// pictures that start decoding afterwards; the host primes a new decoder with one pull before the first push.
void Decoder::request_stages(uint32_t plane_stage_mask) {
#ifdef OVT_DE265_INTERNALS
  const uint32_t wanted = plane_stage_mask & kSnapshotStageMask;
  if ((wanted & ~snapshot_stages_) == 0) {
    return;
  }
  if (snapshot_stages_ == 0) {
    de265_internals_set_plane_snapshots(as_ctx(ctx_), 1);
  }
  snapshot_stages_ |= wanted;
#else
  (void)plane_stage_mask;  // a system libde265 has no snapshots; caps announce Reconstruction only
#endif
}

bool Decoder::pop(Frame* out) {
  if (frames_.empty()) {
    return false;
  }
  *out = std::move(frames_.front());
  frames_.pop_front();
  if (held_back_) {
    (void)decode_loop();  // a failure was recorded as a Diagnostic of the next frame
  }
  return true;
}

ovt_status Decoder::decode_loop() {
  held_back_ = false;
  ovt_status status = OVT_OK;
  int consecutive_errors = 0;
  for (int call = 0; call < kMaxDecodeCalls; ++call) {
    if (frames_.size() >= kMaxPendingFrames) {
      held_back_ = true;
      return status;
    }
    int more = 0;
    const de265_error err = de265_decode(as_ctx(ctx_), &more);
    collect_warnings();
    const std::size_t before = frames_.size();
    drain_pictures();  // before the next de265_decode: libde265 recycles its pictures
    const bool drained = frames_.size() != before;
    if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA) {
      return status;
    }
    if (err == DE265_ERROR_IMAGE_BUFFER_FULL) {
      if (drained) {
        continue;
      }
      add_diag(kError, "DEC-H265-0001", "libde265: decoded picture buffer full");
      return OVT_E_CORRUPT;
    }
    if (de265_isOK(err) == 0) {
      add_diag(kError, "DEC-H265-0001", std::string("libde265: ") + de265_get_error_text(err));
      status = err == DE265_ERROR_OUT_OF_MEMORY ? OVT_E_NOMEM : OVT_E_CORRUPT;
      if (++consecutive_errors >= kMaxConsecutiveErrors) {
        return status;
      }
      continue;  // libde265 dropped the failing unit; decode what is left
    }
    consecutive_errors = 0;
    // A drained picture means the output queue may hold more (decctx.cc reports `more` = 0 once the reorder buffer
    // is flushed at the end of the stream even when that flush just queued pictures); keep going like dec265 does.
    if (more == 0 && !drained) {
      return status;
    }
  }
  add_diag(kError, "DEC-H265-0001", "libde265: decode loop made no progress");
  return OVT_E_CORRUPT;
}

void Decoder::collect_warnings() {
  for (int i = 0; i < 64; ++i) {
    const de265_error w = de265_get_warning(as_ctx(ctx_));
    if (w == DE265_OK) {
      return;
    }
    add_diag(kWarning, "", std::string("libde265: ") + de265_get_error_text(w));
  }
}

void Decoder::drain_pictures() {
  de265_decoder_context* ctx = as_ctx(ctx_);
  while (const de265_image* img = de265_peek_next_picture(ctx)) {
    Frame f;
    copy_picture(img, &f);
    de265_release_next_picture(ctx);
    frames_.push_back(std::move(f));
  }
}

namespace {

// Copies the planes of @p img (the whole decoded picture when @p buf names its buffer) into @p frame.
void copy_planes(const de265_image* img, const Buffer* buf, Frame* frame) {
  const de265_chroma chroma = de265_get_chroma_format(img);
  frame->chroma_format = static_cast<uint8_t>(chroma);  // de265_chroma_mono/420/422/444 = 0..3
  const int sub_w = (chroma == de265_chroma_420 || chroma == de265_chroma_422) ? 2 : 1;
  const int sub_h = chroma == de265_chroma_420 ? 2 : 1;
  const int planes = chroma == de265_chroma_mono ? 1 : 3;
  for (int c = 0; c < planes; ++c) {
    int stride = 0;
    const uint8_t* visible = de265_get_image_plane(img, c, &stride);
    const int bits = de265_get_bits_per_pixel(img, c);
    const int bytes = bits > 8 ? 2 : 1;
    const int w = c == 0 ? frame->coded_w : frame->coded_w / sub_w;
    const int h = c == 0 ? frame->coded_h : frame->coded_h / sub_h;
    const uint8_t* origin = buf != nullptr ? buf->planes[static_cast<std::size_t>(c)] : visible;
    if (origin == nullptr || w <= 0 || h <= 0 || stride < w * bytes) {
      continue;
    }
    Plane& p = frame->planes[static_cast<std::size_t>(c)];
    p.width = static_cast<uint32_t>(w);
    p.height = static_cast<uint32_t>(h);
    p.bit_depth = static_cast<uint8_t>(bits);
    p.sample_bytes = static_cast<uint8_t>(bytes);
    const std::size_t row = static_cast<std::size_t>(w) * static_cast<std::size_t>(bytes);
    const std::span<const uint8_t> src(origin,
                                       (static_cast<std::size_t>(stride) * static_cast<std::size_t>(h - 1)) + row);
    p.data.resize(row * static_cast<std::size_t>(h));
    auto dst = p.data.begin();
    for (std::size_t y = 0; y < static_cast<std::size_t>(h); ++y) {
      const std::span<const uint8_t> line = src.subspan(y * static_cast<std::size_t>(stride), row);
      dst = std::copy(line.begin(), line.end(), dst);
    }
  }
  frame->bit_depth_luma = frame->planes[0].bit_depth;
  frame->bit_depth_chroma = planes == 3 ? frame->planes[1].bit_depth : 0;
}

#ifdef OVT_DE265_INTERNALS
// POC, slice count, slice QP and the slice-majority type from the patch-0001 internals API.
void read_internals(const de265_image* img, Frame* frame) {
  frame->poc = de265_internals_get_picture_poc(img);
  const int slices = de265_internals_get_num_slice_headers(img);
  frame->slice_count = static_cast<uint16_t>(std::clamp(slices, 0, 0xFFFF));
  std::array<int, 3> per_type{};  // H.265 slice_type: 0 B, 1 P, 2 I
  for (int i = 0; i < slices; ++i) {
    de265_internals_slice_header sh{};
    de265_internals_get_slice_header(img, i, &sh);
    if (i == 0) {
      frame->qp_base = static_cast<int16_t>(sh.SliceQPY);
    }
    if (sh.slice_type >= 0 && sh.slice_type <= 2) {
      ++per_type[static_cast<std::size_t>(sh.slice_type)];
    }
  }
  // FrameInfo.type is the slice majority (ovt_model.fbs); a tie goes to the type with more prediction.
  if (slices > 0) {
    const int b = per_type[0];
    const int p = per_type[1];
    const int i = per_type[2];
    frame->frame_type = (b >= p && b >= i) ? 2 : (p >= i ? 1 : 0);
  }
}
#endif

}  // namespace

void Decoder::copy_picture(const de265_image* img, Frame* frame) {
  frame->decode_index = static_cast<int32_t>(reinterpret_cast<intptr_t>(de265_get_image_user_data(img)));
  if (const auto it = pts_.find(frame->decode_index); it != pts_.end()) {
    frame->pts = it->second;
    pts_.erase(it);
  }

  int nal_type = 0;
  int layer_id = 0;
  int temporal_id = 0;
  de265_get_image_NAL_header(img, &nal_type, nullptr, &layer_id, &temporal_id);
  frame->is_rap = nal_type >= 16 && nal_type <= 23;  // IRAP: BLA, IDR, CRA (H.265 Table 7-1)
  frame->layer_id = static_cast<uint8_t>(layer_id);
  frame->temporal_id = static_cast<uint8_t>(temporal_id);

  const auto* buf = static_cast<const Buffer*>(de265_get_image_plane_user_data(img, 0));
  if (buf != nullptr) {
    frame->coded_w = buf->width;
    frame->coded_h = buf->height;
    frame->crop_x = buf->crop_left;
    frame->crop_y = buf->crop_top;
    frame->crop_w = buf->visible_width;
    frame->crop_h = buf->visible_height;
  } else {  // not allocated by get_buffer(): only the cropped picture is known
    frame->coded_w = frame->crop_w = de265_get_image_width(img, 0);
    frame->coded_h = frame->crop_h = de265_get_image_height(img, 0);
  }
  copy_planes(img, buf, frame);
  if (frame->chroma_format != de265_chroma_420 || frame->bit_depth_luma > 10 || frame->bit_depth_chroma > 10) {
    add_diag(kWarning, "DEC-H265-0010", kUseHm);  // outside Main/Main 10 although the profile check passed
  }
#ifdef OVT_DE265_INTERNALS
  read_internals(img, frame);
  if (!copy_snapshots(img, snapshot_stages_, frame)) {  // also frees libde265's snapshots of the picture
    add_diag(kInfo, "",
             "libde265: plane snapshots left out: only the cropped picture is available without worker threads");
  }
#endif
  frame->diagnostics = std::move(pending_diags_);
  pending_diags_.clear();
}

}  // namespace ovt::de265
