// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 uncompressed_header() (chapter 7 §7.3.6 row 11; WI-1.117 up to and including delta_lf_params() and the
// lossless derivations, WI-1.118 rows 20–27 through film_grain_params()). The function mirrors AV1 §5.9.2 line by line.
// It reads bits and consults the DecoderState of WI-1.119 (RefFrameType for show_existing_frame, RefValid /
// RefOrderHint for ref_order_hint[], the slots for frame_size_with_refs(), set_frame_refs() and load_previous()); it
// mutates the state only where §5.9.2 does inside the header (the shown-KEY reset, mark_ref_frames(), the
// ref_order_hint mismatch), never the §7.20 / §7.21 processes, which the index runs after the frame's tile groups.

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecUncompressedHeader = "AV1 5.9.2";
constexpr std::string_view kSpecTemporalPointInfo = "AV1 5.9.31";
constexpr std::string_view kSpecInterpolationFilter = "AV1 5.9.10";

// idLen of AV1 §5.9.2: the width of current_frame_id / display_frame_id, 0 without frame id numbers.
unsigned id_len(const SequenceHeader& seq) noexcept {
  if (!seq.frame_id_numbers_present_flag) {
    return 0;
  }
  return seq.additional_frame_id_length_minus_1 + seq.delta_frame_id_length_minus_2 + 3U;
}

TemporalPointInfo parse_temporal_point_info(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq) {
  // AV1 §5.9.31
  TemporalPointInfo t;
  const auto start = br.mark();
  auto s = b.scoped("temporal_point_info", kSpecTemporalPointInfo);
  const unsigned n = seq.decoder_model_info.frame_presentation_time_length_minus_1 + 1U;
  t.frame_presentation_time = static_cast<uint32_t>(b.read_u("frame_presentation_time", n));
  b.set_descriptor_text("f(n)");
  t.present = true;
  t.span = br.span_since(start);
  return t;
}

bool temporal_point_info_present(const SequenceHeader& seq) noexcept {
  return seq.decoder_model_info_present_flag && !seq.timing_info.equal_picture_interval;
}

// The show_existing_frame == 1 branch of AV1 §5.9.2: everything after frame_to_show_map_idx, then return.
void parse_show_existing(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, const DecoderState& state,
                         FrameHeader& h) {
  h.frame_to_show_map_idx = static_cast<uint8_t>(b.read_u("frame_to_show_map_idx", 3));
  if (temporal_point_info_present(seq)) {
    h.temporal_point_info = parse_temporal_point_info(br, b, seq);
  }
  h.refresh_frame_flags = 0;
  if (seq.frame_id_numbers_present_flag) {
    h.display_frame_id = static_cast<uint32_t>(b.read_u("display_frame_id", id_len(seq)));
    b.set_descriptor_text("f(idLen)");
  }
  const RefSlot& slot = state.slot(h.frame_to_show_map_idx);
  h.frame_type = slot.RefFrameType;
  h.FrameIsIntra = frame_is_intra(h.frame_type);
  h.show_frame = true;
  h.showable_frame = slot.RefShowableFrame;
  if (h.frame_type == FrameType::Key) {
    h.refresh_frame_flags = kAllFrames;
  }
  // load_grain_params(frame_to_show_map_idx) and the §7.21 loading process restore the slot; the slot model runs
  // them (RefValid is its SYN-AV1-0013 check). The header carries the slot's state for the analysis path.
  h.state = slot.load();
  h.is_rap = false;  // a show_existing_frame of a KEY slot is is_rap in the slot model (§7.4.3)
  b.derived("show_existing", fmt::format("frame_type = {}, refresh_frame_flags = {}, RefValid = {}, OrderHint = {}",
                                         frame_type_name(h.frame_type), h.refresh_frame_flags, slot.RefValid ? 1 : 0,
                                         slot.RefOrderHint));
}

// buffer_removal_time_present_flag and the per-operating-point buffer_removal_time[opNum] (AV1 §5.9.2).
void parse_buffer_removal_times(SyntaxBuilder& b, const SequenceHeader& seq, uint8_t temporal_id, uint8_t spatial_id,
                                FrameHeader& h) {
  h.buffer_removal_time_present_flag = b.read_flag("buffer_removal_time_present_flag");
  if (!h.buffer_removal_time_present_flag) {
    return;
  }
  const unsigned n = seq.decoder_model_info.buffer_removal_time_length_minus_1 + 1U;
  for (size_t opNum = 0; opNum < seq.operating_points.size(); ++opNum) {
    const OperatingPoint& op = seq.operating_points[opNum];
    if (!op.decoder_model_present_for_this_op) {
      continue;
    }
    const uint16_t opPtIdc = op.operating_point_idc;
    const bool inTemporalLayer = ((opPtIdc >> temporal_id) & 1U) != 0;
    const bool inSpatialLayer = ((opPtIdc >> (spatial_id + 8U)) & 1U) != 0;
    if (opPtIdc == 0 || (inTemporalLayer && inSpatialLayer)) {
      b.begin_array_element("buffer_removal_time", static_cast<int>(opNum), kSpecUncompressedHeader);
      const auto value = static_cast<uint32_t>(b.read_u("buffer_removal_time", n));
      b.set_descriptor_text("f(n)");
      b.end_array_element();
      h.buffer_removal_time.push_back(BufferRemovalTime{static_cast<uint32_t>(opNum), value});
    }
  }
}

// ref_order_hint[i] (AV1 §5.9.2, error_resilient_mode && enable_order_hint): a mismatch with RefOrderHint[i]
// means the decoder would substitute a grey frame (AV1 §6.8.2); OVT clears RefValid[i], takes the signalled hint
// and records Diagnostic Info SYN-AV1-0011.
void parse_ref_order_hints(SyntaxBuilder& b, const SequenceHeader& seq, DecoderState& state, FrameHeader& h) {
  h.ref_order_hint_present = true;
  for (int i = 0; i < kNumRefFrames; ++i) {
    b.begin_array_element("ref_order_hint", i, kSpecUncompressedHeader);
    const auto hint = static_cast<uint32_t>(b.read_u("ref_order_hint", seq.OrderHintBits));
    b.set_descriptor_text("f(OrderHintBits)");
    h.ref_order_hint[static_cast<size_t>(i)] = hint;
    RefSlot& slot = state.slot(i);
    if (hint != slot.RefOrderHint) {
      b.diag(Severity::Info, kDiagRefOrderHintMismatch,
             fmt::format("ref_order_hint mismatch: RefValid[{}] cleared (signalled {}, slot {})", i, hint,
                         slot.RefOrderHint),
             kSpecUncompressedHeader, true);
      slot.RefValid = false;
      slot.RefOrderHint = hint;
    }
    b.end_array_element();
  }
}

// The reference selection of an inter frame (AV1 §5.9.2): frame_refs_short_signaling, ref_frame_idx[],
// delta_frame_id_minus_1[], the frame size, interpolation filter, motion mode and ref frame mvs flags.
void parse_inter_refs(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, DecoderState& state, FrameHeader& h,
                      const DiagnosticCallback& state_diag) {
  if (!seq.enable_order_hint) {
    h.frame_refs_short_signaling = false;
  } else {
    h.frame_refs_short_signaling = b.read_flag("frame_refs_short_signaling");
  }
  if (h.frame_refs_short_signaling) {
    h.last_frame_idx = static_cast<uint8_t>(b.read_u("last_frame_idx", 3));
    h.gold_frame_idx = static_cast<uint8_t>(b.read_u("gold_frame_idx", 3));
    h.ref_frame_idx = set_frame_refs(state, h.state.OrderHint, h.last_frame_idx, h.gold_frame_idx,
                                     seq.enable_order_hint, seq.OrderHintBits, state_diag);
  }
  const unsigned idLen = id_len(seq);
  for (int i = 0; i < kRefsPerFrame; ++i) {
    const auto si = static_cast<size_t>(i);
    b.begin_array_element("ref_frame_idx", i, kSpecUncompressedHeader);
    if (!h.frame_refs_short_signaling) {
      h.ref_frame_idx[si] = static_cast<int>(b.read_u("ref_frame_idx", 3));
    } else {
      b.derived("ref_frame_idx", fmt::format("ref_frame_idx[{}] = {} (set_frame_refs)", i, h.ref_frame_idx[si]));
    }
    b.set_enum(ref_frame_name(i + kLastFrame));
    if (seq.frame_id_numbers_present_flag) {
      const unsigned bits = seq.delta_frame_id_length_minus_2 + 2U;
      h.delta_frame_id_minus_1[si] = static_cast<uint32_t>(b.read_u("delta_frame_id_minus_1", bits));
      b.set_descriptor_text("f(n)");
      const uint64_t DeltaFrameId = h.delta_frame_id_minus_1[si] + 1ULL;
      const uint64_t range = 1ULL << idLen;
      h.expectedFrameId[si] = static_cast<uint32_t>((h.current_frame_id + range - DeltaFrameId) % range);
      b.derived("expectedFrameId", h.expectedFrameId[si]);
    }
    b.end_array_element();
  }
  if (h.frame_size_override_flag && !h.error_resilient_mode) {
    h.frame_size_with_refs = parse_frame_size_with_refs(br, b, seq, state, h.ref_frame_idx, h.frame_size_override_flag,
                                                        h.frame_size, h.render_size);
    h.frame_size_with_refs_present = true;
  } else {
    h.frame_size = parse_frame_size(br, b, seq, h.frame_size_override_flag);
    h.render_size = parse_render_size(br, b, h.frame_size);
  }
  if (h.force_integer_mv) {
    h.allow_high_precision_mv = false;
  } else {
    h.allow_high_precision_mv = b.read_flag("allow_high_precision_mv");
  }
  {
    // read_interpolation_filter() (AV1 §5.9.10)
    auto s = b.scoped("read_interpolation_filter", kSpecInterpolationFilter);
    h.is_filter_switchable = b.read_flag("is_filter_switchable");
    if (h.is_filter_switchable) {
      h.interpolation_filter = InterpolationFilter::Switchable;
      b.derived("interpolation_filter", "interpolation_filter = SWITCHABLE");
    } else {
      h.interpolation_filter = static_cast<InterpolationFilter>(b.read_u("interpolation_filter", 2));
      b.set_enum(interpolation_filter_name(h.interpolation_filter));
    }
  }
  h.is_motion_mode_switchable = b.read_flag("is_motion_mode_switchable");
  if (h.error_resilient_mode || !seq.enable_ref_frame_mvs) {
    h.use_ref_frame_mvs = false;
  } else {
    h.use_ref_frame_mvs = b.read_flag("use_ref_frame_mvs");
  }
  const RefOrderHints hints =
      compute_order_hints(state, h.ref_frame_idx, h.state.OrderHint, seq.enable_order_hint, seq.OrderHintBits);
  h.state.OrderHints = hints.OrderHints;
  h.RefFrameSignBias = hints.RefFrameSignBias;
  std::string text = "OrderHints = {";
  for (int r = kLastFrame; r <= kAltrefFrame; ++r) {
    text += fmt::format("{}{}:{}{}", r == kLastFrame ? "" : ", ", ref_frame_name(r),
                        hints.OrderHints[static_cast<size_t>(r)],
                        hints.RefFrameSignBias[static_cast<size_t>(r)] ? "-" : "");
  }
  b.derived("OrderHints", text + "} (- = RefFrameSignBias)");
}

// setup_past_independence() / load_previous() (AV1 §5.9.2 with §7.20): the loop filter deltas, feature tables
// and gm_params the frame starts from. FrameState's defaults are the setup_past_independence() values.
void load_previous(const DecoderState& state, FrameHeader& h) {
  if (h.primary_ref_frame == kPrimaryRefNone) {
    h.state.loop_filter_ref_deltas = FrameState{}.loop_filter_ref_deltas;
    h.state.loop_filter_mode_deltas = FrameState{}.loop_filter_mode_deltas;
    h.state.FeatureEnabled = FeatureEnabledTable{};
    h.state.FeatureData = FeatureDataTable{};
    h.state.gm_params = identity_gm_params();
    return;
  }
  const int prevFrame = h.ref_frame_idx[static_cast<size_t>(h.primary_ref_frame)];
  if (prevFrame < 0 || prevFrame >= kNumRefFrames) {
    return;
  }
  const RefSlot& slot = state.slot(prevFrame);
  h.state.loop_filter_ref_deltas = slot.SavedLoopFilterRefDeltas;
  h.state.loop_filter_mode_deltas = slot.SavedLoopFilterModeDeltas;
  h.state.FeatureEnabled = slot.SavedFeatureEnabled;
  h.state.FeatureData = slot.SavedFeatureData;
  h.state.gm_params = slot.SavedGmParams;  // PrevGmParams
}

void emit_summary(SyntaxBuilder& b, const FrameHeader& h) {
  b.derived("frame_summary",
            fmt::format("FrameType = {}, shown = {}, showable_frame = {}, is_rap = {}, poc = {}, qp_base = {}, "
                        "coded_size = {}x{}, crop = {}x{}, UpscaledWidth = {}, refresh_frame_flags = 0x{:02X}, "
                        "CodedLossless = {}, AllLossless = {}",
                        frame_type_name(h.frame_type), h.show_frame ? 1 : 0, h.showable_frame ? 1 : 0, h.is_rap ? 1 : 0,
                        h.state.OrderHint, h.quantization_params.base_q_idx, h.frame_size.FrameWidth,
                        h.frame_size.FrameHeight, h.render_size.RenderWidth, h.render_size.RenderHeight,
                        h.frame_size.UpscaledWidth, h.refresh_frame_flags, h.lossless.CodedLossless ? 1 : 0,
                        h.lossless.AllLossless ? 1 : 0));
}

// The first rows of AV1 §5.9.2: show_existing_frame, frame_type, show_frame, temporal_point_info(), showable_frame
// and error_resilient_mode (all inferred under reduced_still_picture_header). Returns true when the header is a
// show_existing_frame header, which parse_show_existing() has then fully handled.
bool parse_frame_kind(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, DecoderState& state, FrameHeader& h) {
  if (seq.reduced_still_picture_header) {
    h.show_existing_frame = false;
    h.frame_type = FrameType::Key;
    h.FrameIsIntra = true;
    h.show_frame = true;
    h.showable_frame = false;
    b.derived("reduced_still_picture_header",
              "show_existing_frame = 0, frame_type = KEY_FRAME, FrameIsIntra = 1, show_frame = 1, showable_frame = 0");
    return false;
  }
  h.show_existing_frame = b.read_flag("show_existing_frame");
  if (h.show_existing_frame) {
    parse_show_existing(br, b, seq, state, h);
    return true;
  }
  h.frame_type = static_cast<FrameType>(b.read_u("frame_type", 2));
  b.set_enum(frame_type_name(h.frame_type));
  h.FrameIsIntra = frame_is_intra(h.frame_type);
  h.show_frame = b.read_flag("show_frame");
  if (h.show_frame && temporal_point_info_present(seq)) {
    h.temporal_point_info = parse_temporal_point_info(br, b, seq);
  }
  if (h.show_frame) {
    h.showable_frame = h.frame_type != FrameType::Key;
  } else {
    h.showable_frame = b.read_flag("showable_frame");
  }
  if (h.frame_type == FrameType::Switch || (h.frame_type == FrameType::Key && h.show_frame)) {
    h.error_resilient_mode = true;
  } else {
    h.error_resilient_mode = b.read_flag("error_resilient_mode");
  }
  return false;
}

// disable_cdf_update, allow_screen_content_tools, force_integer_mv (AV1 §5.9.2 after error_resilient_mode).
void parse_screen_content_tools(SyntaxBuilder& b, const SequenceHeader& seq, FrameHeader& h) {
  h.disable_cdf_update = b.read_flag("disable_cdf_update");
  if (seq.seq_force_screen_content_tools == kSelectScreenContentTools) {
    h.allow_screen_content_tools = b.read_flag("allow_screen_content_tools");
  } else {
    h.allow_screen_content_tools = seq.seq_force_screen_content_tools != 0;
  }
  if (h.allow_screen_content_tools) {
    if (seq.seq_force_integer_mv == kSelectIntegerMv) {
      h.force_integer_mv = b.read_flag("force_integer_mv");
    } else {
      h.force_integer_mv = seq.seq_force_integer_mv != 0;
    }
  } else {
    h.force_integer_mv = false;
  }
  if (h.FrameIsIntra) {
    h.force_integer_mv = true;
  }
}

// current_frame_id with mark_ref_frames(), frame_size_override_flag, order_hint and primary_ref_frame.
void parse_frame_ids_and_order(SyntaxBuilder& b, const SequenceHeader& seq, DecoderState& state, FrameHeader& h) {
  if (seq.frame_id_numbers_present_flag) {
    // PrevFrameID = current_frame_id of the previous frame is a decoder variable the parser does not keep.
    h.current_frame_id = static_cast<uint32_t>(b.read_u("current_frame_id", id_len(seq)));
    b.set_descriptor_text("f(idLen)");
    mark_ref_frames(state, seq, h.current_frame_id, h.frame_type, h.show_frame);
  }
  h.state.current_frame_id = h.current_frame_id;
  if (h.frame_type == FrameType::Switch) {
    h.frame_size_override_flag = true;
  } else if (seq.reduced_still_picture_header) {
    h.frame_size_override_flag = false;
  } else {
    h.frame_size_override_flag = b.read_flag("frame_size_override_flag");
  }
  if (seq.OrderHintBits > 0) {
    h.order_hint = static_cast<uint32_t>(b.read_u("order_hint", seq.OrderHintBits));
    b.set_descriptor_text("f(OrderHintBits)");
  }
  h.state.OrderHint = h.order_hint;
  if (h.FrameIsIntra || h.error_resilient_mode) {
    h.primary_ref_frame = kPrimaryRefNone;
  } else {
    h.primary_ref_frame = static_cast<uint8_t>(b.read_u("primary_ref_frame", 3));
    if (h.primary_ref_frame == kPrimaryRefNone) {
      b.set_enum("PRIMARY_REF_NONE");
    }
  }
}

// refresh_frame_flags, ref_order_hint[], then frame_size()/render_size()/allow_intrabc for intra frames or the
// reference signalling (parse_inter_refs) for inter frames; copies the sizes into the FrameState.
void parse_refresh_and_sizes(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, DecoderState& state,
                             FrameHeader& h, const DiagnosticCallback& state_diag) {
  h.allow_high_precision_mv = false;
  h.use_ref_frame_mvs = false;
  h.allow_intrabc = false;
  if (h.frame_type == FrameType::Switch || (h.frame_type == FrameType::Key && h.show_frame)) {
    h.refresh_frame_flags = kAllFrames;
    b.derived("refresh_frame_flags", "refresh_frame_flags = allFrames (0xFF)");
  } else {
    h.refresh_frame_flags = static_cast<uint8_t>(b.read_u("refresh_frame_flags", 8));
  }
  if ((!h.FrameIsIntra || h.refresh_frame_flags != kAllFrames) && h.error_resilient_mode && seq.enable_order_hint) {
    parse_ref_order_hints(b, seq, state, h);
  }
  if (h.FrameIsIntra) {
    h.frame_size = parse_frame_size(br, b, seq, h.frame_size_override_flag);
    h.render_size = parse_render_size(br, b, h.frame_size);
    if (h.allow_screen_content_tools && h.frame_size.UpscaledWidth == h.frame_size.FrameWidth) {
      h.allow_intrabc = b.read_flag("allow_intrabc");
    }
  } else {
    parse_inter_refs(br, b, seq, state, h, state_diag);
  }
  h.state.UpscaledWidth = h.frame_size.UpscaledWidth;
  h.state.FrameWidth = h.frame_size.FrameWidth;
  h.state.FrameHeight = h.frame_size.FrameHeight;
  h.state.RenderWidth = h.render_size.RenderWidth;
  h.state.RenderHeight = h.render_size.RenderHeight;
  h.state.MiCols = h.frame_size.MiCols;
  h.state.MiRows = h.frame_size.MiRows;
}

// tile_info() … delta_lf_params() and the lossless derivations (§7.3.6 rows 15–19 chained from row 11).
void parse_tiles_quant_segmentation(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, FrameHeader& h) {
  h.tile_info = parse_tile_info(br, b, seq, h.frame_size.MiCols, h.frame_size.MiRows);
  h.quantization_params = parse_quantization_params(br, b, seq.color_config);
  h.segmentation_params =
      parse_segmentation_params(br, b, h.primary_ref_frame, h.state.FeatureEnabled, h.state.FeatureData);
  h.state.FeatureEnabled = h.segmentation_params.FeatureEnabled;
  h.state.FeatureData = h.segmentation_params.FeatureData;
  h.delta_q_params = parse_delta_q_params(br, b, h.quantization_params.base_q_idx);
  h.delta_lf_params = parse_delta_lf_params(br, b, h.delta_q_params.delta_q_present, h.allow_intrabc);
  // init_coeff_cdfs() / load_previous_segment_ids(): no-ops for OVT.
  h.lossless = compute_lossless(h.quantization_params, h.segmentation_params,
                                h.frame_size.FrameWidth == h.frame_size.UpscaledWidth);
  b.derived("lossless", fmt::format("CodedLossless = {}, AllLossless = {}", h.lossless.CodedLossless ? 1 : 0,
                                    h.lossless.AllLossless ? 1 : 0));
}

// loop_filter_params() … film_grain_params() (§7.3.6 rows 20–27, WI-1.118) in AV1 §5.9.2 order, from the values
// load_previous() / setup_past_independence() left in h.state (loop filter deltas, PrevGmParams); the results go
// back into h.state for the §7.20 update process.
void parse_rows_20_to_27(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, const DecoderState& state,
                         FrameHeader& h) {
  const bool CodedLossless = h.lossless.CodedLossless;
  h.loop_filter_params = parse_loop_filter_params(br, b, seq.color_config, CodedLossless, h.allow_intrabc,
                                                  h.state.loop_filter_ref_deltas, h.state.loop_filter_mode_deltas);
  h.state.loop_filter_ref_deltas = h.loop_filter_params.loop_filter_ref_deltas;
  h.state.loop_filter_mode_deltas = h.loop_filter_params.loop_filter_mode_deltas;
  h.cdef_params = parse_cdef_params(br, b, seq, CodedLossless, h.allow_intrabc);
  h.lr_params = parse_lr_params(br, b, seq, h.lossless.AllLossless, h.allow_intrabc, h.frame_size.UpscaledWidth,
                                h.frame_size.FrameHeight);
  h.tx_mode = parse_tx_mode(br, b, CodedLossless);
  h.reference_select = parse_frame_reference_mode(br, b, h.FrameIsIntra);
  h.skip_mode_params =
      parse_skip_mode_params(br, b, seq, state, h.ref_frame_idx, h.state.OrderHint, h.FrameIsIntra, h.reference_select);
  if (h.FrameIsIntra || h.error_resilient_mode || !seq.enable_warped_motion) {
    h.allow_warped_motion = false;
  } else {
    h.allow_warped_motion = b.read_flag("allow_warped_motion");
  }
  h.reduced_tx_set = b.read_flag("reduced_tx_set");
  h.global_motion_params =
      parse_global_motion_params(br, b, h.FrameIsIntra, h.allow_high_precision_mv, h.state.gm_params);
  h.state.gm_params = h.global_motion_params.gm_params;
  h.state.film_grain_params =
      parse_film_grain_params(br, b, seq, state, h.ref_frame_idx, h.frame_type, h.show_frame, h.showable_frame);
}

FrameHeader parse_uncompressed_header(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, DecoderState& state,
                                      uint8_t temporal_id, uint8_t spatial_id) {
  FrameHeader h;
  const auto start = br.mark();
  auto s = b.scoped("uncompressed_header", kSpecUncompressedHeader);
  const DiagnosticCallback state_diag = [&b](const StateDiagnostic& d) {
    b.diag(Severity::Error, d.code, d.message, kSpecUncompressedHeader);
  };
  const ColorConfig& cc = seq.color_config;
  h.state.subsampling_x = cc.subsampling_x;
  h.state.subsampling_y = cc.subsampling_y;
  h.state.BitDepth = cc.BitDepth;

  if (parse_frame_kind(br, b, seq, state, h)) {
    h.span = br.span_since(start);
    return h;
  }
  h.state.frame_type = h.frame_type;
  h.state.showable_frame = h.showable_frame;

  if (h.frame_type == FrameType::Key && h.show_frame) {
    // AV1 §5.9.2: RefValid[i] = 0, RefOrderHint[i] = 0, OrderHints[LAST_FRAME + i] = 0 (§7.3.8 reset).
    state.reset_for_shown_key_frame();
    h.state.OrderHints.fill(0);
    b.derived("shown_key_frame_reset", "RefValid[0..7] = 0, RefOrderHint[0..7] = 0, OrderHints[LAST..ALTREF] = 0");
  }

  parse_screen_content_tools(b, seq, h);
  parse_frame_ids_and_order(b, seq, state, h);
  if (seq.decoder_model_info_present_flag) {
    parse_buffer_removal_times(b, seq, temporal_id, spatial_id, h);
  }
  parse_refresh_and_sizes(br, b, seq, state, h, state_diag);

  if (seq.reduced_still_picture_header || h.disable_cdf_update) {
    h.disable_frame_end_update_cdf = true;
  } else {
    h.disable_frame_end_update_cdf = b.read_flag("disable_frame_end_update_cdf");
  }
  load_previous(state, h);  // setup_past_independence() or load_previous(); CDFs are not modelled
  // motion_field_estimation() (use_ref_frame_mvs) is decoder-only and not performed (§7.3.6 row 11).
  parse_tiles_quant_segmentation(br, b, seq, h);
  parse_rows_20_to_27(br, b, seq, state, h);
  h.is_rap = h.frame_type == FrameType::Key && h.show_frame;
  emit_summary(b, h);
  h.span = br.span_since(start);
  return h;
}

}  // namespace

std::string_view interpolation_filter_name(InterpolationFilter f) noexcept {
  switch (f) {
    case InterpolationFilter::EightTap:
      return "EIGHTTAP";
    case InterpolationFilter::EightTapSmooth:
      return "EIGHTTAP_SMOOTH";
    case InterpolationFilter::EightTapSharp:
      return "EIGHTTAP_SHARP";
    case InterpolationFilter::Bilinear:
      return "BILINEAR";
    case InterpolationFilter::Switchable:
    default:
      return "SWITCHABLE";
  }
}

void mark_ref_frames(DecoderState& state, const SequenceHeader& seq, uint32_t current_frame_id, FrameType frame_type,
                     bool show_frame) noexcept {
  // AV1 §5.9.4 mark_ref_frames(idLen)
  const unsigned idLen = id_len(seq);
  const unsigned diffLen = seq.delta_frame_id_length_minus_2 + 2U;
  const uint64_t id = current_frame_id;
  const uint64_t diffRange = 1ULL << diffLen;
  const uint64_t idRange = 1ULL << idLen;
  for (int i = 0; i < kNumRefFrames; ++i) {
    RefSlot& slot = state.slot(i);
    if (frame_type == FrameType::Key && show_frame) {
      slot.RefValid = false;
    } else if (id > diffRange) {
      if (slot.RefFrameId > id || slot.RefFrameId < id - diffRange) {
        slot.RefValid = false;
      }
    } else {
      if (slot.RefFrameId > id && slot.RefFrameId < idRange + id - diffRange) {
        slot.RefValid = false;
      }
    }
  }
}

FrameHeader parse_frame_header(BitReader& br, SyntaxBuilder& b, const ObuHeader& obu, const SequenceHeader& seq,
                               DecoderState& state, bool is_redundant) {
  if (is_redundant) {
    // frame_header_copy() (§7.3.6 row 10): the redundant header is parsed against the state as it was, without
    // changing it; the caller compares the result with the frame's first header (SYN-AV1-0010).
    DecoderState copy = state;
    return parse_uncompressed_header(br, b, seq, copy, obu.temporal_id, obu.spatial_id);
  }
  return parse_uncompressed_header(br, b, seq, state, obu.temporal_id, obu.spatial_id);
}

FrameHeader parse_frame_header(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, DecoderState& state,
                               bool is_redundant) {
  // #513 (ruled): the §7.3.7 signature has no OBU layer ids; an OBU without extension header is layer 0.
  const ObuHeader obu;
  return parse_frame_header(br, b, obu, seq, state, is_redundant);
}

}  // namespace ovt::syntax::av1
