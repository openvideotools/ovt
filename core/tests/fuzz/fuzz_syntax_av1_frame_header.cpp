// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the AV1 uncompressed_header() sub-structure parsers (chapter 7 §7.3.6 rows 12–19, chapter 14
// §14.7.1, WI-1.117): frame_size / superres_params / render_size, frame_size_with_refs, tile_info,
// quantization_params, segmentation_params, delta_q_params and delta_lf_params, and the whole uncompressed_header()
// of row 11 (parse_frame_header, selected by bit 7 of byte 1 in case 3). Byte 0 selects the parser and the
// sequence header shape (superblock size, superres, frame size bits, chroma layout), byte 1 the frame-level
// inputs (override flag, primary_ref_frame, MiCols/MiRows scale, slot validity); the rest is the bit stream. Both
// recording modes run and must return the same typed values. A ParseError is the expected answer to malformed
// bytes; any other exception, a crash or a sanitizer report is a finding.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/frame_header.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
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
  seq.use_128x128_superblock = (sel & 0x08U) != 0;
  seq.enable_superres = (sel & 0x10U) != 0;
  seq.frame_width_bits_minus_1 = static_cast<uint8_t>((sel & 0x20U) != 0 ? 15 : 8);
  seq.frame_height_bits_minus_1 = static_cast<uint8_t>((sel & 0x20U) != 0 ? 15 : 8);
  seq.max_frame_width_minus_1 = (sel & 0x40U) != 0 ? 8191U : 351U;
  seq.max_frame_height_minus_1 = (sel & 0x40U) != 0 ? 4351U : 287U;
  seq.max_frame_width = seq.max_frame_width_minus_1 + 1;
  seq.max_frame_height = seq.max_frame_height_minus_1 + 1;
  seq.color_config.NumPlanes = (sel & 0x80U) != 0 ? 1 : 3;
  seq.color_config.mono_chrome = seq.color_config.NumPlanes == 1;
  seq.color_config.separate_uv_delta_q = (sel & 0x04U) != 0;
  return seq;
}

DecoderState make_state(uint8_t ctl) {
  DecoderState state;
  for (int i = 0; i < kNumRefFrames; ++i) {
    if (((ctl >> 4U) & (1U << (static_cast<unsigned>(i) & 3U))) != 0) {
      FrameState fs;
      fs.UpscaledWidth = 64U * static_cast<uint32_t>(i + 1);
      fs.FrameWidth = fs.UpscaledWidth;
      fs.FrameHeight = 48U * static_cast<uint32_t>(i + 1);
      fs.RenderWidth = fs.UpscaledWidth;
      fs.RenderHeight = fs.FrameHeight;
      state.slot(i).save(fs, i);
    }
  }
  return state;
}

uint64_t mix(uint64_t h, uint64_t v) {
  return (h ^ v) * 0x9E3779B97F4A7C15ULL;
}

// Case 3 with bit 7 of the control byte: the whole uncompressed_header() (row 11) over the slots of make_state().
// The sequence shape adds order hints, frame ids and the screen-content / integer-mv selectors so that every
// branch of AV1 §5.9.2 is reachable.
uint64_t run_frame_header(ovt::BitReader& br, ovt::syntax::SyntaxBuilder& b, const SequenceHeader& seq,
                          const DecoderState& state, uint8_t sel, uint8_t ctl) {
  SequenceHeader fseq = seq;
  fseq.enable_order_hint = (ctl & 0x01U) != 0;
  fseq.order_hint_bits_minus_1 = 6;
  fseq.OrderHintBits = fseq.enable_order_hint ? 7 : 0;
  fseq.enable_ref_frame_mvs = fseq.enable_order_hint;
  fseq.frame_id_numbers_present_flag = (ctl & 0x02U) != 0;
  fseq.delta_frame_id_length_minus_2 = 12;
  fseq.additional_frame_id_length_minus_1 = 0;
  fseq.reduced_still_picture_header = (ctl & 0x04U) != 0;
  fseq.seq_force_screen_content_tools = (ctl & 0x08U) != 0 ? kSelectScreenContentTools : 0;
  fseq.seq_force_integer_mv = (ctl & 0x08U) != 0 ? kSelectIntegerMv : 0;
  DecoderState st = state;
  ObuHeader obu;
  obu.obu_type = static_cast<uint8_t>(ObuType::FrameHeader);
  obu.temporal_id = static_cast<uint8_t>(sel & 0x07U);
  obu.spatial_id = static_cast<uint8_t>((sel >> 3U) & 0x03U);
  const FrameHeader h = parse_frame_header(br, b, obu, fseq, st, false);
  const uint64_t summary = mix(mix(static_cast<uint64_t>(h.frame_type), h.show_existing_frame ? 1U : 0U),
                               mix(h.refresh_frame_flags, mix(h.frame_size.MiCols, h.tile_info.NumTiles)));
  return mix(summary, mix(h.quantization_params.base_q_idx, h.lossless.CodedLossless ? 1U : 0U));
}

// Runs the parser selected by the two control bytes on the remaining input; returns a summary of the typed
// result so the two recording modes can be compared.
uint64_t run(std::span<const uint8_t> input, bool recording) {
  const uint8_t sel = input[0];
  const uint8_t ctl = input[1];
  const SequenceHeader seq = make_sequence(sel);
  const DecoderState state = make_state(ctl);
  const bool override_flag = (ctl & 0x01U) != 0;
  const auto primary_ref_frame = static_cast<uint8_t>((ctl & 0x02U) != 0 ? kPrimaryRefNone : (ctl >> 2U) & 7U);
  const uint32_t MiCols = ((ctl & 0x0FU) + 1U) * 16U * ((sel & 0x40U) != 0 ? 8U : 1U);
  const uint32_t MiRows = (static_cast<uint32_t>(ctl >> 4U) + 1U) * 8U;
  ovt::BitReader br(input.subspan(2));
  ovt::syntax::SyntaxBuilder b(br);
  b.set_recording(recording);
  uint64_t summary = 0;
  try {
    switch (sel & 0x03U) {
      case 0: {
        FrameSize fs = parse_frame_size(br, b, seq, override_flag);
        const RenderSize rs = parse_render_size(br, b, fs);
        summary = mix(mix(mix(fs.FrameWidth, fs.MiCols), fs.SuperresDenom), mix(rs.RenderWidth, rs.RenderHeight));
        RefFrameIdx idx{};
        for (int i = 0; i < kRefsPerFrame; ++i) {
          idx[static_cast<size_t>(i)] = (ctl + i) % kNumRefFrames;
        }
        RenderSize rs2;
        const FrameSizeWithRefs r = parse_frame_size_with_refs(br, b, seq, state, idx, override_flag, fs, rs2);
        summary = mix(summary, mix(static_cast<uint64_t>(r.found_ref_index + 1), mix(fs.MiRows, rs2.RenderHeight)));
        break;
      }
      case 1: {
        const TileInfo t = parse_tile_info(br, b, seq, MiCols, MiRows);
        require(t.MiColStarts.size() == t.TileCols + 1U);
        require(t.MiRowStarts.size() == t.TileRows + 1U);
        summary = mix(mix(t.NumTiles, t.context_update_tile_id), mix(t.MiColStarts.back(), t.MiRowStarts.back()));
        (void)t.tile_rect(0, 0);
        (void)t.tile_rect(t.TileRows, t.TileCols);
        break;
      }
      case 2: {
        const QuantizationParams q = parse_quantization_params(br, b, seq.color_config);
        const SegmentationParams seg =
            parse_segmentation_params(br, b, primary_ref_frame, FeatureEnabledTable{}, FeatureDataTable{});
        const Lossless l = compute_lossless(q, seg, override_flag);
        summary = mix(mix(q.base_q_idx, static_cast<uint64_t>(static_cast<int64_t>(q.DeltaQVAc) + 64)),
                      mix(seg.LastActiveSegId, l.CodedLossless ? 1U : 0U));
        break;
      }
      default: {
        if ((ctl & 0x80U) != 0) {
          summary = run_frame_header(br, b, seq, state, sel, ctl);
          break;
        }
        const DeltaQParams dq = parse_delta_q_params(br, b, ctl);
        const DeltaLfParams dlf = parse_delta_lf_params(br, b, dq.delta_q_present, override_flag);
        summary =
            mix(mix(dq.delta_q_res, dq.delta_q_present ? 1U : 0U), mix(dlf.delta_lf_res, dlf.delta_lf_multi ? 1U : 0U));
        break;
      }
    }
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
  if (size < 2) {
    return 0;
  }
  const std::span<const uint8_t> input(data, size);
  require(run(input, true) == run(input, false));
  return 0;
}
