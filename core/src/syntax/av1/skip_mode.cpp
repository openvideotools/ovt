// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 frame_reference_mode() and skip_mode_params() (chapter 7 §7.3.6 rows 24 and 25; WI-1.118). The SkipModeFrame
// search mirrors AV1 §5.9.22 line by line on the RefOrderHint[] of the slots named by ref_frame_idx[].

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecReferenceMode = "AV1 5.9.23";
constexpr std::string_view kSpecSkipMode = "AV1 5.9.22";

// RefOrderHint[ref_frame_idx[i]]; an unset entry (-1, an intra frame's array) reads as 0.
uint32_t ref_hint(const DecoderState& state, const RefFrameIdx& ref_frame_idx, int i) noexcept {
  const int slot = ref_frame_idx[static_cast<size_t>(i)];
  if (slot < 0 || slot >= kNumRefFrames) {
    return 0;
  }
  return state.slot(slot).RefOrderHint;
}

// The SkipModeFrame search of AV1 §5.9.22 (the else branch: inter frame, reference_select, enable_order_hint).
void find_skip_mode_frames(const SequenceHeader& seq, const DecoderState& state, const RefFrameIdx& ref_frame_idx,
                           uint32_t OrderHint, SkipModeParams& sm) {
  const auto dist = [&](uint32_t a, uint32_t c) {
    return get_relative_dist(a, c, seq.enable_order_hint, seq.OrderHintBits);
  };
  int forwardIdx = -1;
  int backwardIdx = -1;
  uint32_t forwardHint = 0;
  uint32_t backwardHint = 0;
  for (int i = 0; i < kRefsPerFrame; ++i) {
    const uint32_t refHint = ref_hint(state, ref_frame_idx, i);
    if (dist(refHint, OrderHint) < 0) {
      if (forwardIdx < 0 || dist(refHint, forwardHint) > 0) {
        forwardIdx = i;
        forwardHint = refHint;
      }
    } else if (dist(refHint, OrderHint) > 0) {
      if (backwardIdx < 0 || dist(refHint, backwardHint) < 0) {
        backwardIdx = i;
        backwardHint = refHint;
      }
    }
  }
  sm.forwardIdx = static_cast<int8_t>(forwardIdx);
  sm.backwardIdx = static_cast<int8_t>(backwardIdx);
  if (forwardIdx < 0) {
    sm.skipModeAllowed = false;
    return;
  }
  int secondIdx = backwardIdx;
  if (backwardIdx < 0) {
    uint32_t secondForwardHint = 0;
    for (int i = 0; i < kRefsPerFrame; ++i) {
      const uint32_t refHint = ref_hint(state, ref_frame_idx, i);
      if (dist(refHint, forwardHint) < 0 && (secondIdx < 0 || dist(refHint, secondForwardHint) > 0)) {
        secondIdx = i;
        secondForwardHint = refHint;
      }
    }
    sm.secondForwardIdx = static_cast<int8_t>(secondIdx);
  }
  if (secondIdx < 0) {
    sm.skipModeAllowed = false;
    return;
  }
  sm.skipModeAllowed = true;
  sm.SkipModeFrame[0] = static_cast<int8_t>(kLastFrame + std::min(forwardIdx, secondIdx));
  sm.SkipModeFrame[1] = static_cast<int8_t>(kLastFrame + std::max(forwardIdx, secondIdx));
}

}  // namespace

bool parse_frame_reference_mode(BitReader& /*br*/, SyntaxBuilder& b, bool FrameIsIntra) {
  // AV1 §5.9.23
  auto s = b.scoped("frame_reference_mode", kSpecReferenceMode);
  if (FrameIsIntra) {
    b.derived("reference_select", "reference_select = 0 (FrameIsIntra)");
    return false;
  }
  return b.read_flag("reference_select");
}

SkipModeParams parse_skip_mode_params(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                      const DecoderState& state, const RefFrameIdx& ref_frame_idx, uint32_t OrderHint,
                                      bool FrameIsIntra, bool reference_select) {
  // AV1 §5.9.22
  SkipModeParams sm;
  const auto start = br.mark();
  auto s = b.scoped("skip_mode_params", kSpecSkipMode);
  if (FrameIsIntra || !reference_select || !seq.enable_order_hint) {
    sm.skipModeAllowed = false;
  } else {
    find_skip_mode_frames(seq, state, ref_frame_idx, OrderHint, sm);
  }
  if (sm.skipModeAllowed) {
    b.derived("SkipModeFrame", fmt::format("skipModeAllowed = 1, SkipModeFrame = {{{}, {}}}",
                                           ref_frame_name(sm.SkipModeFrame[0]), ref_frame_name(sm.SkipModeFrame[1])));
    sm.skip_mode_present = b.read_flag("skip_mode_present");
  } else {
    b.derived("skip_mode_present", "skipModeAllowed = 0, skip_mode_present = 0");
    sm.skip_mode_present = false;
  }
  sm.span = br.span_since(start);
  return sm;
}

}  // namespace ovt::syntax::av1
