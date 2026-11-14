// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 reference slot processes (chapter 7 §7.3.10.4, WI-1.119): get_relative_dist() (AV1 §5.9.3), the reference
// frame update process (§7.20), the reference frame loading process (§7.21), set_frame_refs() (§7.8) and the
// OrderHints / RefFrameSignBias derivation (§5.9.2). The code follows the pseudo-code of §7.3.10.4 line by line.

#include "ovt/cxx/syntax/av1/decoder_state.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace ovt::syntax::av1 {

namespace {

constexpr int32_t kWarpedModelPrecBits = 16;  // WARPEDMODEL_PREC_BITS (AV1 §3)

void report(const DiagnosticCallback& diag, std::string_view code, std::string message) {
  if (diag) {
    diag(StateDiagnostic{code, std::move(message)});
  }
}

}  // namespace

std::string_view ref_frame_name(int ref_frame) noexcept {
  switch (ref_frame) {
    case kIntraFrame:
      return "INTRA";
    case kLastFrame:
      return "LAST";
    case kLast2Frame:
      return "LAST2";
    case kLast3Frame:
      return "LAST3";
    case kGoldenFrame:
      return "GOLDEN";
    case kBwdrefFrame:
      return "BWDREF";
    case kAltref2Frame:
      return "ALTREF2";
    case kAltrefFrame:
      return "ALTREF";
    default:
      return "NONE";
  }
}

std::string_view frame_type_name(FrameType t) noexcept {
  switch (t) {
    case FrameType::Key:
      return "KEY_FRAME";
    case FrameType::Inter:
      return "INTER_FRAME";
    case FrameType::IntraOnly:
      return "INTRA_ONLY_FRAME";
    case FrameType::Switch:
      return "SWITCH_FRAME";
  }
  return "KEY_FRAME";
}

GmParams identity_gm_params() noexcept {
  GmParams gm{};
  for (auto& ref : gm) {
    for (size_t i = 0; i < ref.size(); ++i) {
      ref[i] = (i % 3 == 2) ? (int32_t{1} << kWarpedModelPrecBits) : 0;
    }
  }
  return gm;
}

// ---- RefSlot ---------------------------------------------------------------------------------------------------

void RefSlot::save(const FrameState& cur, int64_t decode_index) noexcept {
  RefValid = true;
  RefFrameId = cur.current_frame_id;
  RefUpscaledWidth = cur.UpscaledWidth;
  RefFrameWidth = cur.FrameWidth;
  RefFrameHeight = cur.FrameHeight;
  RefRenderWidth = cur.RenderWidth;
  RefRenderHeight = cur.RenderHeight;
  RefMiCols = cur.MiCols;
  RefMiRows = cur.MiRows;
  RefFrameType = cur.frame_type;
  RefSubsamplingX = cur.subsampling_x;
  RefSubsamplingY = cur.subsampling_y;
  RefBitDepth = cur.BitDepth;
  RefOrderHint = cur.OrderHint;
  RefShowableFrame = cur.showable_frame;
  SavedOrderHints = cur.OrderHints;
  SavedGmParams = cur.gm_params;
  SavedLoopFilterRefDeltas = cur.loop_filter_ref_deltas;
  SavedLoopFilterModeDeltas = cur.loop_filter_mode_deltas;
  SavedFeatureEnabled = cur.FeatureEnabled;
  SavedFeatureData = cur.FeatureData;
  SavedFilmGrainParams = cur.film_grain_params;
  slot_decode_index = decode_index;
}

FrameState RefSlot::load() const noexcept {
  FrameState cur;
  cur.current_frame_id = RefFrameId;
  cur.UpscaledWidth = RefUpscaledWidth;
  cur.FrameWidth = RefFrameWidth;
  cur.FrameHeight = RefFrameHeight;
  cur.RenderWidth = RefRenderWidth;
  cur.RenderHeight = RefRenderHeight;
  cur.MiCols = RefMiCols;
  cur.MiRows = RefMiRows;
  cur.frame_type = RefFrameType;
  cur.subsampling_x = RefSubsamplingX;
  cur.subsampling_y = RefSubsamplingY;
  cur.BitDepth = RefBitDepth;
  cur.OrderHint = RefOrderHint;
  cur.showable_frame = RefShowableFrame;
  cur.OrderHints = SavedOrderHints;
  cur.gm_params = SavedGmParams;
  cur.loop_filter_ref_deltas = SavedLoopFilterRefDeltas;
  cur.loop_filter_mode_deltas = SavedLoopFilterModeDeltas;
  cur.FeatureEnabled = SavedFeatureEnabled;
  cur.FeatureData = SavedFeatureData;
  cur.film_grain_params = SavedFilmGrainParams;
  return cur;
}

// ---- DecoderState ------------------------------------------------------------------------------------------------

void DecoderState::reset_for_shown_key_frame() noexcept {
  for (RefSlot& s : slots) {
    s.RefValid = false;
    s.RefOrderHint = 0;
    s.slot_decode_index = -1;
  }
}

void DecoderState::clear() noexcept {
  for (RefSlot& s : slots) {
    s = RefSlot{};
  }
}

std::array<int64_t, kNumRefFrames> DecoderState::slot_decode_indices() const noexcept {
  std::array<int64_t, kNumRefFrames> out{};
  for (size_t i = 0; i < slots.size(); ++i) {
    out[i] = slots[i].RefValid ? slots[i].slot_decode_index : -1;
  }
  return out;
}

// ---- processes -----------------------------------------------------------------------------------------------------

int32_t get_relative_dist(uint32_t a, uint32_t b, bool enable_order_hint, uint8_t OrderHintBits) noexcept {
  if (!enable_order_hint || OrderHintBits == 0) {
    return 0;
  }
  // diff = a - b; m = 1 << (OrderHintBits - 1); diff = (diff & (m - 1)) - (diff & m)
  const uint32_t diff = a - b;  // modular, as the spec's integer arithmetic on OrderHintBits-wide values
  const uint32_t m = uint32_t{1} << (OrderHintBits - 1);
  return static_cast<int32_t>(diff & (m - 1)) - static_cast<int32_t>(diff & m);
}

void reference_frame_update(DecoderState& state, const FrameState& cur, uint8_t refresh_frame_flags,
                            int64_t decode_index, const DiagnosticCallback& diag) {
  if (cur.frame_type == FrameType::IntraOnly && refresh_frame_flags == kAllFrames) {
    report(diag, kDiagIntraOnlyRefreshAll, "intra_only frame with refresh_frame_flags = 0xFF");
  }
  for (int i = 0; i < kNumRefFrames; ++i) {
    if (((refresh_frame_flags >> i) & 1U) != 0) {
      state.slot(i).save(cur, decode_index);
    }
  }
}

bool reference_frame_loading(const DecoderState& state, uint8_t frame_to_show_map_idx, FrameState& out,
                             const DiagnosticCallback& diag) {
  const int s = frame_to_show_map_idx & (kNumRefFrames - 1);  // frame_to_show_map_idx is f(3)
  const RefSlot& slot = state.slot(s);
  out = slot.load();
  if (!slot.RefValid) {
    report(diag, kDiagShowExistingEmptySlot, fmt::format("show_existing_frame of an empty slot ({})", s));
    return false;
  }
  return true;
}

namespace {

struct FrameRefsSearch {
  std::array<int32_t, kNumRefFrames> shiftedOrderHints{};
  std::array<bool, kNumRefFrames> usedFrame{};
  int32_t curFrameHint = 0;

  // find_latest_backward(): largest shiftedOrderHints[i] >= curFrameHint among !usedFrame.
  [[nodiscard]] int latest_backward() const noexcept {
    int ref = -1;
    int32_t latestOrderHint = 0;
    for (int i = 0; i < kNumRefFrames; ++i) {
      const int32_t hint = shiftedOrderHints[static_cast<size_t>(i)];
      if (!usedFrame[static_cast<size_t>(i)] && hint >= curFrameHint && (ref < 0 || hint >= latestOrderHint)) {
        ref = i;
        latestOrderHint = hint;
      }
    }
    return ref;
  }
  // find_earliest_backward(): smallest shiftedOrderHints[i] >= curFrameHint among !usedFrame.
  [[nodiscard]] int earliest_backward() const noexcept {
    int ref = -1;
    int32_t earliestOrderHint = 0;
    for (int i = 0; i < kNumRefFrames; ++i) {
      const int32_t hint = shiftedOrderHints[static_cast<size_t>(i)];
      if (!usedFrame[static_cast<size_t>(i)] && hint >= curFrameHint && (ref < 0 || hint < earliestOrderHint)) {
        ref = i;
        earliestOrderHint = hint;
      }
    }
    return ref;
  }
  // find_latest_forward(): largest shiftedOrderHints[i] < curFrameHint among !usedFrame.
  [[nodiscard]] int latest_forward() const noexcept {
    int ref = -1;
    int32_t latestOrderHint = 0;
    for (int i = 0; i < kNumRefFrames; ++i) {
      const int32_t hint = shiftedOrderHints[static_cast<size_t>(i)];
      if (!usedFrame[static_cast<size_t>(i)] && hint < curFrameHint && (ref < 0 || hint >= latestOrderHint)) {
        ref = i;
        latestOrderHint = hint;
      }
    }
    return ref;
  }
};

}  // namespace

RefFrameIdx set_frame_refs(const DecoderState& state, uint32_t OrderHint, uint8_t last_frame_idx,
                           uint8_t gold_frame_idx, bool enable_order_hint, uint8_t OrderHintBits,
                           const DiagnosticCallback& diag) {
  RefFrameIdx ref_frame_idx{};
  ref_frame_idx.fill(-1);
  const auto last = static_cast<size_t>(last_frame_idx & (kNumRefFrames - 1));  // f(3) values
  const auto gold = static_cast<size_t>(gold_frame_idx & (kNumRefFrames - 1));
  ref_frame_idx[0] = static_cast<int>(last);  // LAST_FRAME - LAST_FRAME
  ref_frame_idx[kGoldenFrame - kLastFrame] = static_cast<int>(gold);

  FrameRefsSearch s;
  s.usedFrame[last] = true;
  s.usedFrame[gold] = true;
  // curFrameHint = 1 << (OrderHintBits - 1). frame_refs_short_signaling requires enable_order_hint, so
  // OrderHintBits >= 1; a caller that violates that gets the degenerate (all zero) hints of OrderHintBits = 1.
  const uint8_t bits = OrderHintBits == 0 ? uint8_t{1} : OrderHintBits;
  s.curFrameHint = int32_t{1} << (bits - 1);
  for (int i = 0; i < kNumRefFrames; ++i) {
    s.shiftedOrderHints[static_cast<size_t>(i)] =
        s.curFrameHint + get_relative_dist(state.slot(i).RefOrderHint, OrderHint, enable_order_hint, OrderHintBits);
  }
  const int32_t lastOrderHint = s.shiftedOrderHints[last];
  if (lastOrderHint >= s.curFrameHint) {
    report(diag, kDiagShortSignalingOrderHint,
           fmt::format("frame_refs_short_signaling: LAST order hint not before current (last_frame_idx {})", last));
  }
  const int32_t goldOrderHint = s.shiftedOrderHints[gold];
  if (goldOrderHint >= s.curFrameHint) {
    report(diag, kDiagShortSignalingOrderHint,
           fmt::format("frame_refs_short_signaling: GOLDEN order hint not before current (gold_frame_idx {})", gold));
  }

  int ref = s.latest_backward();
  if (ref >= 0) {
    ref_frame_idx[kAltrefFrame - kLastFrame] = ref;
    s.usedFrame[static_cast<size_t>(ref)] = true;
  }
  ref = s.earliest_backward();
  if (ref >= 0) {
    ref_frame_idx[kBwdrefFrame - kLastFrame] = ref;
    s.usedFrame[static_cast<size_t>(ref)] = true;
  }
  ref = s.earliest_backward();
  if (ref >= 0) {
    ref_frame_idx[kAltref2Frame - kLastFrame] = ref;
    s.usedFrame[static_cast<size_t>(ref)] = true;
  }
  constexpr std::array<RefFrame, 5> kRefFrameList{kLast2Frame, kLast3Frame, kBwdrefFrame, kAltref2Frame, kAltrefFrame};
  for (const RefFrame refFrame : kRefFrameList) {
    if (ref_frame_idx[static_cast<size_t>(refFrame - kLastFrame)] < 0) {
      ref = s.latest_forward();
      if (ref >= 0) {
        ref_frame_idx[static_cast<size_t>(refFrame - kLastFrame)] = ref;
        s.usedFrame[static_cast<size_t>(ref)] = true;
      }
    }
  }
  // Finally, any remaining references are set to the reference frame with smallest output order.
  ref = -1;
  int32_t earliestOrderHint = 0;
  for (int i = 0; i < kNumRefFrames; ++i) {
    const int32_t hint = s.shiftedOrderHints[static_cast<size_t>(i)];
    if (ref < 0 || hint < earliestOrderHint) {
      ref = i;
      earliestOrderHint = hint;
    }
  }
  for (auto& idx : ref_frame_idx) {
    if (idx < 0) {
      idx = ref;
    }
  }
  return ref_frame_idx;
}

RefOrderHints compute_order_hints(const DecoderState& state, const RefFrameIdx& ref_frame_idx, uint32_t OrderHint,
                                  bool enable_order_hint, uint8_t OrderHintBits) noexcept {
  RefOrderHints out;
  for (int refFrame = kLastFrame; refFrame <= kAltrefFrame; ++refFrame) {
    const int idx = ref_frame_idx[static_cast<size_t>(refFrame - kLastFrame)];
    if (idx < 0 || idx >= kNumRefFrames) {
      continue;  // an unset entry (intra frame): OrderHints 0, sign bias 0
    }
    const uint32_t hint = state.slot(idx).RefOrderHint;
    out.OrderHints[static_cast<size_t>(refFrame)] = hint;
    out.RefFrameSignBias[static_cast<size_t>(refFrame)] =
        enable_order_hint && get_relative_dist(hint, OrderHint, enable_order_hint, OrderHintBits) > 0;
  }
  return out;
}

}  // namespace ovt::syntax::av1
