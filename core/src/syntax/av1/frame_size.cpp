// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 frame_size(), superres_params(), compute_image_size(), render_size() and frame_size_with_refs() (chapter 7
// §7.3.6 rows 12–14; WI-1.117). The functions mirror AV1 §5.9.5 – §5.9.9 line by line; the values the tables
// infer are written into the structs and emitted as derived nodes so the tree shows what a decoder would assume.

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecFrameSize = "AV1 5.9.5";
constexpr std::string_view kSpecRenderSize = "AV1 5.9.6";
constexpr std::string_view kSpecFrameSizeWithRefs = "AV1 5.9.7";
constexpr std::string_view kSpecSuperres = "AV1 5.9.8";

// The conformance requirement of AV1 §5.9.5 (frame_width_minus_1 <= max_frame_width_minus_1, same for the
// height). SPEC-ASSUMPTION(#513): §7.9 registers no code for a frame-header value outside its allowed range
// that is not a reserved value; SYN-AV1-0015 is proposed. The values stand, as a decoder would keep them.
void check_against_sequence_maximum(SyntaxBuilder& b, const SequenceHeader& seq, const FrameSize& fs) {
  if (fs.frame_width_minus_1 > seq.max_frame_width_minus_1 || fs.frame_height_minus_1 > seq.max_frame_height_minus_1) {
    b.diag(Severity::Error, kDiagFrameHeaderRange,
           fmt::format("frame size {}x{} exceeds the sequence maximum {}x{}", fs.frame_width_minus_1 + 1U,
                       fs.frame_height_minus_1 + 1U, seq.max_frame_width, seq.max_frame_height),
           kSpecFrameSize);
  }
}

void emit_image_size(SyntaxBuilder& b, const FrameSize& fs) {
  b.derived("image_size", fmt::format("UpscaledWidth = {}, FrameWidth = {}, FrameHeight = {}, MiCols = {}, MiRows = {}",
                                      fs.UpscaledWidth, fs.FrameWidth, fs.FrameHeight, fs.MiCols, fs.MiRows));
}

}  // namespace

void compute_image_size(FrameSize& fs) noexcept {
  // AV1 §5.9.9
  fs.MiCols = 2U * ((fs.FrameWidth + 7U) >> 3U);
  fs.MiRows = 2U * ((fs.FrameHeight + 7U) >> 3U);
}

void parse_superres_params(SyntaxBuilder& b, const SequenceHeader& seq, FrameSize& fs) {
  // AV1 §5.9.8
  auto s = b.scoped("superres_params", kSpecSuperres);
  if (seq.enable_superres) {
    fs.use_superres = b.read_flag("use_superres");
  } else {
    fs.use_superres = false;
  }
  if (fs.use_superres) {
    fs.coded_denom = static_cast<uint8_t>(b.read_u("coded_denom", kSuperresDenomBits));
    fs.SuperresDenom = static_cast<uint8_t>(fs.coded_denom + kSuperresDenomMin);
  } else {
    fs.coded_denom = 0;
    fs.SuperresDenom = kSuperresNum;
  }
  fs.UpscaledWidth = fs.FrameWidth;
  fs.FrameWidth = (fs.UpscaledWidth * kSuperresNum + (fs.SuperresDenom / 2U)) / fs.SuperresDenom;
  b.derived("SuperresDenom", fs.SuperresDenom);
}

FrameSize parse_frame_size(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, bool frame_size_override_flag) {
  // AV1 §5.9.5
  FrameSize fs;
  const auto start = br.mark();
  auto s = b.scoped("frame_size", kSpecFrameSize);
  if (frame_size_override_flag) {
    fs.frame_width_minus_1 = static_cast<uint32_t>(b.read_u("frame_width_minus_1", seq.frame_width_bits_minus_1 + 1U));
    b.set_descriptor_text("f(n)");
    fs.frame_height_minus_1 =
        static_cast<uint32_t>(b.read_u("frame_height_minus_1", seq.frame_height_bits_minus_1 + 1U));
    b.set_descriptor_text("f(n)");
    fs.size_from_sequence_header = false;
    check_against_sequence_maximum(b, seq, fs);
  } else {
    fs.frame_width_minus_1 = seq.max_frame_width_minus_1;
    fs.frame_height_minus_1 = seq.max_frame_height_minus_1;
    fs.size_from_sequence_header = true;
    b.derived("frame_size_from_sequence_header",
              fmt::format("frame_width_minus_1 = {}, frame_height_minus_1 = {} (max_frame_*_minus_1)",
                          fs.frame_width_minus_1, fs.frame_height_minus_1));
  }
  fs.FrameWidth = fs.frame_width_minus_1 + 1U;
  fs.FrameHeight = fs.frame_height_minus_1 + 1U;
  parse_superres_params(b, seq, fs);
  compute_image_size(fs);
  emit_image_size(b, fs);
  fs.span = br.span_since(start);
  return fs;
}

RenderSize parse_render_size(BitReader& br, SyntaxBuilder& b, const FrameSize& fs) {
  // AV1 §5.9.6
  RenderSize rs;
  const auto start = br.mark();
  auto s = b.scoped("render_size", kSpecRenderSize);
  rs.render_and_frame_size_different = b.read_flag("render_and_frame_size_different");
  if (rs.render_and_frame_size_different) {
    rs.render_width_minus_1 = static_cast<uint32_t>(b.read_u("render_width_minus_1", 16));
    rs.render_height_minus_1 = static_cast<uint32_t>(b.read_u("render_height_minus_1", 16));
    rs.RenderWidth = rs.render_width_minus_1 + 1U;
    rs.RenderHeight = rs.render_height_minus_1 + 1U;
  } else {
    rs.RenderWidth = fs.UpscaledWidth;
    rs.RenderHeight = fs.FrameHeight;
    rs.render_width_minus_1 = rs.RenderWidth == 0 ? 0 : rs.RenderWidth - 1U;
    rs.render_height_minus_1 = rs.RenderHeight == 0 ? 0 : rs.RenderHeight - 1U;
  }
  b.derived("render_size", fmt::format("RenderWidth = {}, RenderHeight = {}", rs.RenderWidth, rs.RenderHeight));
  rs.span = br.span_since(start);
  return rs;
}

FrameSizeWithRefs parse_frame_size_with_refs(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                             const DecoderState& state, const RefFrameIdx& ref_frame_idx,
                                             bool frame_size_override_flag, FrameSize& fs, RenderSize& rs) {
  // AV1 §5.9.7
  FrameSizeWithRefs out;
  const auto start = br.mark();
  auto s = b.scoped("frame_size_with_refs", kSpecFrameSizeWithRefs);
  for (int i = 0; i < kRefsPerFrame; ++i) {
    b.begin_array_element("found_ref", i, kSpecFrameSizeWithRefs);
    const bool found = b.read_flag("found_ref");
    b.end_array_element();
    out.found_ref[static_cast<size_t>(i)] = found;
    if (!found) {
      continue;
    }
    out.found_ref_index = static_cast<int8_t>(i);
    const int idx = ref_frame_idx[static_cast<size_t>(i)];
    const int slot_index = (idx >= 0 && idx < kNumRefFrames) ? idx : 0;
    const RefSlot& slot = state.slot(slot_index);
    if (idx < 0 || idx >= kNumRefFrames || !slot.RefValid) {
      // SPEC-ASSUMPTION(#513): a found_ref into an empty slot has no §7.9 code; SYN-AV1-0015 proposed. The
      // stale slot values are used, as a decoder would.
      b.diag(Severity::Error, kDiagFrameHeaderRange,
             fmt::format("found_ref[{}]: ref_frame_idx[{}] = {} is an empty reference slot", i, i, idx),
             kSpecFrameSizeWithRefs, true);
    }
    fs = FrameSize{};
    fs.UpscaledWidth = slot.RefUpscaledWidth;
    fs.FrameWidth = fs.UpscaledWidth;
    fs.FrameHeight = slot.RefFrameHeight;
    fs.frame_width_minus_1 = fs.UpscaledWidth == 0 ? 0 : fs.UpscaledWidth - 1U;
    fs.frame_height_minus_1 = fs.FrameHeight == 0 ? 0 : fs.FrameHeight - 1U;
    rs = RenderSize{};
    rs.RenderWidth = slot.RefRenderWidth;
    rs.RenderHeight = slot.RefRenderHeight;
    rs.render_width_minus_1 = rs.RenderWidth == 0 ? 0 : rs.RenderWidth - 1U;
    rs.render_height_minus_1 = rs.RenderHeight == 0 ? 0 : rs.RenderHeight - 1U;
    rs.render_and_frame_size_different = rs.RenderWidth != fs.UpscaledWidth || rs.RenderHeight != fs.FrameHeight;
    b.derived("size_from_ref",
              fmt::format("ref_frame_idx[{}] = {}: UpscaledWidth = {}, FrameHeight = {}, RenderWidth = {}, "
                          "RenderHeight = {}",
                          i, idx, fs.UpscaledWidth, fs.FrameHeight, rs.RenderWidth, rs.RenderHeight));
    break;
  }
  if (out.found_ref_index < 0) {
    fs = parse_frame_size(br, b, seq, frame_size_override_flag);
    rs = parse_render_size(br, b, fs);
  } else {
    parse_superres_params(b, seq, fs);
    compute_image_size(fs);
    emit_image_size(b, fs);
    b.derived("render_size", fmt::format("RenderWidth = {}, RenderHeight = {}", rs.RenderWidth, rs.RenderHeight));
  }
  out.span = br.span_since(start);
  return out;
}

}  // namespace ovt::syntax::av1
