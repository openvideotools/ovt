// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 slot model step of the index (chapter 7 §7.4.3, §7.4.4 "AV1 (slot model, §7.20/§7.21)", WI-1.119).
// The builder integration (declared_refs/dpb_after into FrameInfo, operating points, Annex B) is WI-1.124.

#include "index/av1_slots.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace ovt::index {

namespace {

using syntax::av1::FrameState;
using syntax::av1::FrameType;
using syntax::av1::kAllFrames;
using syntax::av1::kNumRefFrames;
using syntax::av1::kRefsPerFrame;

void report(const IndexDiagnosticCallback& diag, Severity sev, std::string_view code, std::string message) {
  if (diag) {
    diag(IndexDiagnostic{sev, code, std::move(message)});
  }
}

// The §7.3.10.4 callbacks (StateDiagnostic) are all Errors; forward them as index diagnostics.
syntax::av1::DiagnosticCallback forward(const IndexDiagnosticCallback& diag) {
  if (!diag) {
    return {};
  }
  return [&diag](const syntax::av1::StateDiagnostic& d) { diag(IndexDiagnostic{Severity::Error, d.code, d.message}); };
}

}  // namespace

void Av1SlotModel::set_sequence(bool enable_order_hint, uint8_t OrderHintBits) noexcept {
  enable_order_hint_ = enable_order_hint;
  OrderHintBits_ = enable_order_hint ? OrderHintBits : uint8_t{0};
}

Av1FrameRecord Av1SlotModel::step_show_existing(const Av1FrameHeaderValues& h, Av1FrameRecord r,
                                                const std::function<int16_t(int64_t)>& qp_base_of,
                                                const syntax::av1::DiagnosticCallback& state_diag) {
  // §7.3.9 / §7.4.3: the frame header OBU is the whole frame; the loading process of §7.21 restores the slot.
  const int slot = h.frame_to_show_map_idx & (kNumRefFrames - 1);
  const bool valid = syntax::av1::reference_frame_loading(state_, h.frame_to_show_map_idx, r.state, state_diag);
  r.type = r.state.frame_type;
  r.shown = h.in_operating_point;
  r.show_existing_of = valid ? state_.slot(slot).slot_decode_index : -1;
  r.poc = r.state.OrderHint;
  r.showable_frame = r.state.showable_frame;
  if (valid && qp_base_of) {
    r.qp_base = qp_base_of(r.show_existing_of);
  }
  r.is_rap = valid && r.type == FrameType::Key;
  if (r.is_rap) {
    // AV1 §7.21 + §5.9.2: a shown KEY via show_existing_frame refreshes every slot (refresh_frame_flags =
    // allFrames); the slots now all hold the shown frame, whose decode index is show_existing_of (§7.4.3).
    syntax::av1::reference_frame_update(state_, r.state, kAllFrames, r.show_existing_of, state_diag);
  }
  r.dpb_after = state_.slot_decode_indices();
  return r;
}

Av1FrameRecord Av1SlotModel::step(const Av1FrameHeaderValues& h, int64_t decode_index,
                                  const std::function<int16_t(int64_t)>& qp_base_of,
                                  const IndexDiagnosticCallback& diag) {
  Av1FrameRecord r;
  r.ref_frame_idx.fill(-1);
  const syntax::av1::DiagnosticCallback state_diag = forward(diag);

  if (h.show_existing_frame) {
    return step_show_existing(h, std::move(r), qp_base_of, state_diag);
  }

  const FrameState& cur = h.state;
  r.type = cur.frame_type;
  r.shown = h.show_frame && h.in_operating_point;
  r.is_rap = cur.frame_type == FrameType::Key && h.show_frame;
  r.showable_frame = !h.show_frame && cur.showable_frame;
  r.qp_base = h.base_q_idx;
  r.state = cur;
  if (enable_order_hint_ && OrderHintBits_ > 0) {
    r.poc = cur.OrderHint;
  } else {
    // §7.3.10.4: OrderHintBits == 0 → poc = decode_index so that poc stays strictly increasing (chapter 6 §6.2.5).
    // SPEC-ASSUMPTION(#426): §7.9 registers no code for this Info; SYN-AV1-0103 is proposed.
    r.poc = decode_index;
    report(diag, Severity::Info, kDiagNoOrderHints, "order hints disabled; poc = decode_index");
  }

  if (cur.frame_type == FrameType::Key && h.show_frame) {
    // AV1 §5.9.2: RefValid[i] = 0, RefOrderHint[i] = 0 before anything else of the frame is evaluated (§7.3.8
    // reset of the DecoderState; WI-1.119 notes).
    state_.reset_for_shown_key_frame();
  }

  if (!syntax::av1::frame_is_intra(cur.frame_type)) {
    r.ref_frame_idx = h.ref_frame_idx;
    // declared_refs = unique(slot_decode_index[ref_frame_idx[i]] for i in 0..6, order LAST, LAST2, LAST3, GOLDEN,
    // BWDREF, ALTREF2, ALTREF), dropping -1 entries with Diagnostic Error SYN-AV1-0102.
    for (int i = 0; i < kRefsPerFrame; ++i) {
      const int idx = h.ref_frame_idx[static_cast<size_t>(i)];
      const int64_t ref_decode_index =
          (idx >= 0 && idx < kNumRefFrames && state_.slot(idx).RefValid) ? state_.slot(idx).slot_decode_index : -1;
      if (ref_decode_index < 0) {
        report(diag, Severity::Error, kDiagRefEmptySlot,
               fmt::format("ref_frame_idx[{}] ({}) points to an empty slot ({})", i,
                           syntax::av1::ref_frame_name(i + syntax::av1::kLastFrame), idx));
        continue;
      }
      if (std::find(r.declared_refs.begin(), r.declared_refs.end(), ref_decode_index) == r.declared_refs.end()) {
        r.declared_refs.push_back(ref_decode_index);
      }
    }
    r.order_hints =
        syntax::av1::compute_order_hints(state_, h.ref_frame_idx, cur.OrderHint, enable_order_hint_, OrderHintBits_);
    r.state.OrderHints = r.order_hints.OrderHints;
  }

  syntax::av1::reference_frame_update(state_, r.state, h.refresh_frame_flags, decode_index, state_diag);
  r.dpb_after = state_.slot_decode_indices();
  return r;
}

}  // namespace ovt::index
