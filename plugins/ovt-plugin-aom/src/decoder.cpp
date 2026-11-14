// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// libaom decoder driving of the AV1 analysis plugin (chapter 07 §7.6.1 "Decoder driving", WI-2.155).

#include "decoder.hpp"

#include <aom/aom_decoder.h>
#include <aom/aom_frame_buffer.h>
#include <aom/aom_image.h>
#include <aom/aomdx.h>

// Semi-public and internal: not installed by libaom, included from the pinned source tree (tp::aom_inspection).
// decoder.h gives the AV1_COMMON frame state ifd_inspect() reads; the callback reads a few more of its fields.
#include "av1/decoder/decoder.h"
#include "av1/decoder/inspection.h"
#include "config/aom_config.h"
#if CONFIG_ACCOUNTING
#include "av1/decoder/accounting.h"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
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
constexpr unsigned kObuTileGroup = 4;
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

// Copies the inspection MI grid and the frame-header values the block mapping needs (chapter 07 §7.6.1). Runs inside
// the inspection callback: it only reads the decoder's frame state, as ifd_inspect() does.
// SPEC-ASSUMPTION(#360): superblock size, ref_frame_idx[], tile layout, CDEF strengths and global-motion types come
// from libaom's AV1_COMMON, because the plugin ABI gives the plugin no parsed headers.
void copy_blocks(const insp_frame_data& fd, const AV1Decoder& pbi, BlockData* out) {
  const AV1_COMMON& cm = pbi.common;
  BlockData& d = *out;
  // The grid's dimensions come from the decoder, not from fd: when ifd_inspect() reallocates the grid for a new frame
  // size it stores mi_rows in fd.mi_cols and mi_cols in fd.mi_rows (libaom v3.12.1 inspection.c passes them to
  // ifd_init_mi_rc() in the wrong order). The grid itself is laid out with the decoder's mi_cols.
  const int mi_rows = cm.mi_params.mi_rows;
  const int mi_cols = cm.mi_params.mi_cols;
  d.valid = mi_rows > 0 && mi_cols > 0 && fd.mi_grid != nullptr && fd.mi_rows * fd.mi_cols == mi_rows * mi_cols;
  if (!d.valid) {
    return;
  }
  d.mi_rows = mi_rows;
  d.mi_cols = mi_cols;
  const size_t n = static_cast<size_t>(mi_rows) * static_cast<size_t>(mi_cols);
  const std::span<const insp_mi_data> grid(fd.mi_grid, n);
  d.mi.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const insp_mi_data& s = grid[i];
    MiInfo& m = d.mi[i];
    for (size_t k = 0; k < 2; ++k) {
      m.mv_row.at(k) = s.mv[k].row;
      m.mv_col.at(k) = s.mv[k].col;
      m.ref_frame.at(k) = s.ref_frame[k];
      m.filter.at(k) = s.filter[k];
    }
    m.mode = s.mode;
    m.uv_mode = s.uv_mode;
    m.bsize = s.bsize;
    m.skip = s.skip;
    m.segment_id = s.segment_id;
    m.tx_size = s.tx_size;
    m.cdef_level = s.cdef_level;
    m.cdef_strength = s.cdef_strength;
    m.cfl_alpha_idx = s.cfl_alpha_idx;
    m.cfl_alpha_sign = s.cfl_alpha_sign;
    m.current_qindex = s.current_qindex;
    m.compound_type = s.compound_type;
    m.motion_mode = s.motion_mode;
    m.intrabc = s.intrabc;
    m.palette = s.palette;
    m.uv_palette = s.uv_palette;
  }
  const SequenceHeader& seq = *cm.seq_params;
  d.sb_mi = seq.sb_size == BLOCK_128X128 ? 32 : 16;
  d.monochrome = seq.monochrome != 0;
  d.intra_frame = frame_is_intra_only(&cm) != 0;
  if (!d.intra_frame) {
    for (size_t t = 0; t < d.ref_frame_idx.size(); ++t) {
      d.ref_frame_idx.at(t) = static_cast<int8_t>(cm.remapped_ref_idx[t]);
    }
  }
  for (size_t t = 0; t < d.gm_type.size(); ++t) {
    d.gm_type.at(t) = cm.global_motion[t].wmtype;
    for (size_t k = 0; k < 6; ++k) {
      d.gm_params.at(t).at(k) = cm.global_motion[t].wmmat[k];
    }
  }
  const CommonTileParams& tiles = cm.tiles;
  d.tile_col_starts.assign(static_cast<size_t>(tiles.cols) + 1, 0);
  d.tile_row_starts.assign(static_cast<size_t>(tiles.rows) + 1, 0);
  for (size_t i = 0; i <= static_cast<size_t>(tiles.cols); ++i) {
    d.tile_col_starts[i] = std::min(tiles.col_start_sb[i] << seq.mib_size_log2, mi_cols);
  }
  for (size_t i = 0; i <= static_cast<size_t>(tiles.rows); ++i) {
    d.tile_row_starts[i] = std::min(tiles.row_start_sb[i] << seq.mib_size_log2, mi_rows);
  }
  const CdefInfo& cdef = cm.cdef_info;
  d.cdef_damping = cdef.cdef_damping;
  d.cdef_bits = cdef.cdef_bits;
  // CDEF runs only when the sequence enables it outside coded-lossless and IntraBC frames (read_cdef()).
  if (seq.enable_cdef != 0 && !cm.features.coded_lossless && !cm.features.allow_intrabc) {
    const auto nb = static_cast<size_t>(std::clamp(cdef.nb_cdef_strengths, 0, CDEF_MAX_STRENGTHS));
    const auto y = std::span(cdef.cdef_strengths).first(nb);
    const auto uv = std::span(cdef.cdef_uv_strengths).first(nb);
    d.cdef_strengths.assign(y.begin(), y.end());
    d.cdef_uv_strengths.assign(uv.begin(), uv.end());
  }
  // Super-resolution (AV1 §5.9.8): cm.width is FrameWidth, the coded (downscaled) width the MI grid covers; by the
  // time the callback runs, superres_post_decode() has upscaled the frame buffer to superres_upscaled_width.
  d.superres_denom = cm.superres_scale_denominator;
  d.frame_width = cm.width;
  d.upscaled_width = cm.superres_upscaled_width;
  d.base_qindex = fd.base_qindex;
  d.delta_q_present = fd.delta_q_present_flag != 0;
  d.delta_q_res = fd.delta_q_res;
  for (size_t s = 0; s < d.y_dequant.size(); ++s) {
    d.y_dequant.at(s) = {fd.y_dequant[s][0], fd.y_dequant[s][1]};
  }
}

#if CONFIG_ACCOUNTING
// Copies libaom's accounting of the frame (chapter 07 §7.6.1 accounting row, WI-2.157). fd.accounting points at
// pbi->accounting, which libaom resets when it starts decoding a tile group, so it must be copied here, inside the
// callback. The dictionary only grows over the decoder's life; it is copied whole, so ids index it unchanged.
void copy_accounting(const insp_frame_data& fd, AccountingData* out) {
  const Accounting* acc = fd.accounting;
  if (acc == nullptr) {
    return;
  }
  const AccountingDictionary& dict = acc->syms.dictionary;
  const auto n_strs = static_cast<size_t>(std::clamp(dict.num_strs, 0, MAX_SYMBOL_TYPES));
  out->names.clear();
  for (const char* str : std::span(dict.strs).first(n_strs)) {
    out->names.emplace_back(str != nullptr ? str : "");
  }
  const auto n_syms = static_cast<size_t>(std::max(acc->syms.num_syms, 0));
  out->symbols.resize(n_syms);
  if (n_syms > 0) {
    const std::span<const AccountingSymbol> syms(acc->syms.syms, n_syms);
    std::transform(syms.begin(), syms.end(), out->symbols.begin(), [](const AccountingSymbol& s) {
      return AcctSymbol{s.context.x, s.context.y, s.id, s.bits, s.samples};
    });
  }
  out->valid = true;
}
#endif

}  // namespace

bool Decoder::has_accounting() {
  return CONFIG_ACCOUNTING != 0;
}

struct Decoder::Impl {
  FramePool pool;  // libaom's frame buffers (planes.hpp); must outlive ctx, which releases them in its destroy
  aom_codec_ctx_t ctx{};
  bool open = false;
  insp_frame_data fd{};
  bool fd_init = false;
  // Filled by the inspection callback, read after aom_codec_decode() returns.
  bool fired = false;
  int frame_type = -1;
  bool show_frame = false;
  int base_qindex = 0;
  bool apply_grain = false;
  bool monochrome = false;  // sequence-level, from the last output image
  BlockData blocks;         // filled by the callback

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
  // image it showed. libaom reuses that image's frame buffer once it has released it, at the next
  // aom_codec_decode() at the latest, so the Picture holds the buffer (FramePool) instead of copying the samples.
  Frame take_frame(int32_t decode_index, int64_t pts) {
    Frame f;
    f.decode_index = decode_index;
    f.pts = pts;
    f.frame_type = fb_frame_type(frame_type);
    f.shown = show_frame;
    f.qp_base = static_cast<int16_t>(base_qindex);
    f.apply_grain = apply_grain;
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
      if (!f.shown || !f.recon.empty()) {
        continue;
      }
      monochrome = img->monochrome != 0;
      f.recon = pool.retain(*img);
    }
    f.chroma_format = have_fmt ? chroma_format_of(fmt, monochrome) : 1;
    f.blocks = std::move(blocks);
    blocks = BlockData{};
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
    // read_film_grain_params() leaves the frame's parameters in cm.film_grain_params (zeroed when the sequence
    // has none). Read here: decoder_get_frame() clears apply_grain in the output frame when grain is skipped.
    const auto* pbi = static_cast<const AV1Decoder*>(decoder);
    self->apply_grain = pbi->common.film_grain_params.apply_grain != 0;
    copy_blocks(self->fd, *pbi, &self->blocks);
#if CONFIG_ACCOUNTING
    if (self->blocks.valid) {
      copy_accounting(self->fd, &self->blocks.accounting);
    }
#endif
    self->fired = true;
  }
};

Decoder::~Decoder() = default;

ovt_status Decoder::create(int threads, std::unique_ptr<Decoder>* out, std::string* error) {
  auto dec = std::make_unique<Decoder>();
  dec->impl_ = std::make_unique<Impl>();
  Impl& im = *dec->impl_;
  aom_codec_dec_cfg_t cfg{};
  // The bundled libaom is always built with CONFIG_ACCOUNTING=1 (inspection needs it). Its tile workers record
  // symbols into the one shared pbi->accounting without a lock (decodeframe.c, tile_worker_hook_init), which corrupts
  // the heap on frames with several tiles: decode on one thread.
  // SPEC-ASSUMPTION(#360): §7.6.1 expects row-MT worker threads; they are unusable with accounting on.
  static_cast<void>(threads);
  cfg.threads = 1;
  cfg.allow_lowbitdepth = 1;  // 8-bit streams give 8-bit planes, as aomdec and inspect do
  if (aom_codec_dec_init(&im.ctx, aom_codec_av1_dx(), &cfg, 0) != AOM_CODEC_OK) {
    *error = std::string("aom_codec_dec_init failed: ") + aom_codec_error(&im.ctx);
    return OVT_E_PLUGIN;
  }
  im.open = true;
  // Before the first decode call, as libaom requires: decoded pictures live in the plugin's pool (planes.hpp).
  if (aom_codec_set_frame_buffer_functions(&im.ctx, &FramePool::get, &FramePool::release, &im.pool) != AOM_CODEC_OK) {
    *error = std::string("aom_codec_set_frame_buffer_functions failed: ") + aom_codec_error(&im.ctx);
    return OVT_E_PLUGIN;
  }
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
  if (type == kObuFrame || type == kObuTileGroup) {
    tu_tile_groups_.push_back(tu_.size());
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
  size_t next_frame_obu = 0;   // index into tu_frames_
  size_t next_tile_group = 0;  // index into tu_tile_groups_
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
    // SPEC-ASSUMPTION(#408): the tile-group count alone decides the "last tile group only" note.
    int32_t tile_groups = 0;  // tile groups consumed by this call: those of the one frame it decoded
    while (next_tile_group < tu_tile_groups_.size() && tu_tile_groups_[next_tile_group] < off) {
      ++next_tile_group;
      ++tile_groups;
    }
    if (im.fired) {
      n = std::max(n, 1);
      Frame f = im.take_frame(tu_first_hint_ + pos + n - 1, tu_frames_[std::max<size_t>(next_frame_obu, 1) - 1].pts);
      f.blocks.accounting.tile_groups = std::max(tile_groups, 1);
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
  tu_tile_groups_.clear();
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
