// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the AV1 uncompressed_header() sub-structure parsers of chapter 7 §7.3.6 rows 20–27 (chapter 14
// §14.7.1, WI-1.118): loop_filter_params, cdef_params, lr_params, read_tx_mode, frame_reference_mode,
// skip_mode_params, global_motion_params and film_grain_params. Byte 0 selects the sequence header shape (superblock
// size, planes, subsampling, enable_cdef / enable_restoration / enable_order_hint, film grain), byte 1 the
// frame-level inputs (CodedLossless, AllLossless, allow_intrabc, FrameIsIntra, frame type, shown / showable,
// allow_high_precision_mv), byte 2 seeds the slot order hints, the previous deltas, PrevGmParams and the saved film
// grain; the rest is the bit stream. Every parser runs in syntax order on the same reader; both recording modes run
// and must return the same typed values. A ParseError is the expected answer to malformed bytes; any other
// exception, a crash or a sanitizer report is a finding.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/frame_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using namespace ovt::syntax::av1;  // NOLINT(google-build-using-namespace)

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

SequenceHeader make_sequence(uint8_t sel) {
  SequenceHeader seq;
  seq.use_128x128_superblock = (sel & 0x01U) != 0;
  seq.color_config.mono_chrome = (sel & 0x02U) != 0;
  seq.color_config.NumPlanes = seq.color_config.mono_chrome ? 1 : 3;
  seq.color_config.subsampling_x = (sel & 0x04U) == 0;
  seq.color_config.subsampling_y = (sel & 0x08U) == 0;
  seq.enable_cdef = (sel & 0x10U) != 0;
  seq.enable_restoration = (sel & 0x20U) != 0;
  seq.enable_order_hint = (sel & 0x40U) != 0;
  seq.OrderHintBits = seq.enable_order_hint ? static_cast<uint8_t>(1U + (sel & 0x07U)) : 0;
  seq.film_grain_params_present = (sel & 0x80U) != 0;
  return seq;
}

DecoderState make_state(uint8_t seed) {
  DecoderState state;
  for (int i = 0; i < kNumRefFrames; ++i) {
    FrameState fs;
    fs.OrderHint = static_cast<uint32_t>((seed * (i + 3)) & 0xFF);
    fs.film_grain_params.apply_grain = ((seed >> (i & 7)) & 1U) != 0;
    fs.film_grain_params.num_y_points = static_cast<uint8_t>((seed + i) & 0x0F);  // may exceed 14
    fs.film_grain_params.grain_seed = static_cast<uint16_t>(seed * 257U);
    state.slot(i).save(fs, i);
  }
  return state;
}

uint64_t mix(uint64_t h, uint64_t v) {
  return (h ^ v) * 0x9E3779B97F4A7C15ULL;
}

uint64_t run(std::span<const uint8_t> input, bool recording) {
  const uint8_t sel = input[0];
  const uint8_t ctl = input[1];
  const uint8_t seed = input[2];
  const SequenceHeader seq = make_sequence(sel);
  const DecoderState state = make_state(seed);
  const bool CodedLossless = (ctl & 0x01U) != 0;
  const bool AllLossless = CodedLossless && (ctl & 0x02U) != 0;
  const bool allow_intrabc = (ctl & 0x04U) != 0;
  const auto frame_type = static_cast<FrameType>((ctl >> 3U) & 3U);
  const bool FrameIsIntra = frame_is_intra(frame_type);
  const bool show_frame = (ctl & 0x20U) != 0;
  const bool showable_frame = (ctl & 0x40U) != 0;
  const bool allow_high_precision_mv = (ctl & 0x80U) != 0;
  RefFrameIdx idx{};
  for (int i = 0; i < kRefsPerFrame; ++i) {
    idx[static_cast<size_t>(i)] = FrameIsIntra ? -1 : (seed + i * 3) % kNumRefFrames;
  }
  std::array<int8_t, kTotalRefsPerFrame> prev_ref{};
  for (size_t i = 0; i < prev_ref.size(); ++i) {
    prev_ref[i] = static_cast<int8_t>(static_cast<int>(seed) - 128 + static_cast<int>(i));
  }
  GmParams prev_gm = identity_gm_params();
  for (size_t r = 0; r < prev_gm.size(); ++r) {
    for (size_t i = 0; i < prev_gm[r].size(); ++i) {
      prev_gm[r][i] += static_cast<int32_t>((seed * (r + 1) * (i + 7)) % 4096) * ((seed & 1U) != 0 ? -64 : 64);
    }
  }

  ovt::BitReader br(input.subspan(3));
  ovt::syntax::SyntaxBuilder b(br);
  b.set_recording(recording);
  uint64_t summary = 0;
  try {
    const LoopFilterParams lf =
        parse_loop_filter_params(br, b, seq.color_config, CodedLossless, allow_intrabc, prev_ref, {1, -1});
    summary = mix(summary, mix(lf.loop_filter_level[0], static_cast<uint8_t>(lf.loop_filter_ref_deltas[7])));
    const CdefParams cd = parse_cdef_params(br, b, seq, CodedLossless, allow_intrabc);
    summary = mix(summary, mix(cd.cdef_bits, cd.cdef_uv_sec_strength[7]));
    const LrParams lr = parse_lr_params(br, b, seq, AllLossless, allow_intrabc, 64U + seed * 31U, 48U + seed * 17U);
    summary = mix(summary, mix(lr.LoopRestorationSize[2], lr.LrUnitCols[0] + lr.LrUnitRows[1]));
    const TxModeParams tx = parse_tx_mode(br, b, CodedLossless);
    const bool reference_select = parse_frame_reference_mode(br, b, FrameIsIntra);
    const SkipModeParams sm =
        parse_skip_mode_params(br, b, seq, state, idx, seed & 0x7FU, FrameIsIntra, reference_select);
    require(!sm.skipModeAllowed || (sm.SkipModeFrame[0] >= kLastFrame && sm.SkipModeFrame[1] <= kAltrefFrame &&
                                    sm.SkipModeFrame[0] < sm.SkipModeFrame[1]));
    summary = mix(summary, mix(static_cast<uint64_t>(tx.TxMode),
                               mix(static_cast<uint8_t>(sm.SkipModeFrame[1]), sm.skip_mode_present ? 1U : 0U)));
    const GlobalMotionParams gm = parse_global_motion_params(br, b, FrameIsIntra, allow_high_precision_mv, prev_gm);
    for (const auto& p : gm.gm_params) {
      summary = mix(summary, static_cast<uint32_t>(p[0] ^ p[3]));
    }
    const FilmGrainParams fg = parse_film_grain_params(br, b, seq, state, idx, frame_type, show_frame, showable_frame);
    require(fg.point_y_value.size() >= fg.num_y_points);
    require(fg.point_cb_value.size() >= fg.num_cb_points);
    require(fg.point_cr_value.size() >= fg.num_cr_points);
    summary = mix(summary, mix(fg.grain_seed, fg.num_y_points + fg.num_cr_points));
  } catch (const ovt::ParseError&) {  // NOLINT(bugprone-empty-catch): the expected answer to malformed input
    summary = ~uint64_t{0};
  }
  const ovt::syntax::SyntaxNodeBuffer buf = b.finish();
  if (recording) {
    (void)buf.to_flatbuffer();
    (void)ovt::syntax::to_json(buf);
  }
  return summary;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 3) {
    return 0;
  }
  const std::span<const uint8_t> input(data, size);
  require(run(input, true) == run(input, false));
  return 0;
}
