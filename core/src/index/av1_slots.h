// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The AV1 slot model of the index (chapter 7 §7.4.3 and §7.4.4 "AV1 (slot model, §7.20/§7.21)", WI-1.119): one
// step per frame turns the parsed frame header values and the DecoderState into the FrameInfo fields that depend
// on the reference slots (type, shown, show_existing_of, is_rap, poc, declared_refs, dpb_after). The StreamIndex
// builder (WI-1.121) and its AV1 integration (WI-1.124) call it; the frame header parser (WI-1.117) fills the
// input. Module-internal, not installed.

#ifndef OVT_INDEX_AV1_SLOTS_H
#define OVT_INDEX_AV1_SLOTS_H

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/syntax/av1/decoder_state.hpp"

namespace ovt::index {

/// Severity values equal ovt.fb.Severity.
enum class Severity : uint8_t { Info = 0, Warning = 1, Error = 2, Fatal = 3 };

/// A finding of the slot model or the frame grouper, with its §7.9 code. The caller attaches it to the frame or
/// the unit it is processing.
struct IndexDiagnostic {
  Severity severity;
  std::string_view code;
  std::string message;
};
using IndexDiagnosticCallback = std::function<void(const IndexDiagnostic&)>;  // may be empty

inline constexpr std::string_view kDiagRefEmptySlot = "SYN-AV1-0102";  // Error, §7.4.4
inline constexpr std::string_view kDiagNoOrderHints = "SYN-AV1-0103";  // Info, §7.3.10.4 poc = decode_index

/// The frame header values of uncompressed_header() (AV1 §5.9.2) that one slot step needs. WI-1.117's parser
/// fills it; tests hand-build it. `state` carries the current frame's §7.20 variables (sizes, frame_type,
/// OrderHint, …); for a show_existing_frame only show_existing_frame and frame_to_show_map_idx are read.
struct Av1FrameHeaderValues {
  bool show_existing_frame = false;
  uint8_t frame_to_show_map_idx = 0;
  bool show_frame = true;
  uint8_t refresh_frame_flags = 0;
  // ref_frame_idx[0..6] (LAST_FRAME..ALTREF_FRAME); read only when !frame_is_intra(state.frame_type).
  syntax::av1::RefFrameIdx ref_frame_idx{};
  int16_t base_q_idx = 0;
  // The §7.18 drop rule result (check_operating_point of obu_header.hpp): a frame outside the selected operating
  // point is indexed with shown = false (§7.4.3 last row).
  bool in_operating_point = true;
  syntax::av1::FrameState state;
};

/// What the slot step decided for one frame (the §7.4.3 table columns).
struct Av1FrameRecord {
  syntax::av1::FrameType type = syntax::av1::FrameType::Key;  // frame_type, or RefFrameType[slot] for show_existing
  bool shown = false;
  int64_t show_existing_of = -1;  // slot_decode_index[frame_to_show_map_idx], or -1
  bool is_rap = false;            // (KEY && show_frame) || (show_existing_frame && RefFrameType[slot] == KEY)
  int64_t poc = 0;                // OrderHint (RefOrderHint[slot] for show_existing); decode_index without order hints
  int16_t qp_base = 0;            // base_q_idx, or the shown frame's qp_base for show_existing
  bool showable_frame = false;    // derived "showable_frame = 1" of a hidden frame
  std::vector<int64_t> declared_refs;                           // §7.4.4: unique slot decode indices, LAST..ALTREF
  std::array<int64_t, syntax::av1::kNumRefFrames> dpb_after{};  // slot_decode_index[0..7] after the frame
  // For the plugin host and the GOP graph (§7.4.4): the slot each reference name resolves to and its order hint.
  syntax::av1::RefFrameIdx ref_frame_idx{};  // -1 for an intra or show_existing frame
  syntax::av1::RefOrderHints order_hints;    // OrderHints[refFrame], RefFrameSignBias[refFrame]
  // The loaded frame state of a show_existing_frame (AV1 §7.21), for the analysis path.
  syntax::av1::FrameState state;
};

/// The per-stream slot model. Owns the DecoderState of §7.3.10.4 and applies §7.3.8 (reset on a shown KEY frame)
/// and §7.4.3 / §7.4.4 per frame.
class Av1SlotModel {
 public:
  /// enable_order_hint / OrderHintBits of the active sequence header (sequence_header.hpp: SequenceHeader
  /// .enable_order_hint, .OrderHintBits). A new sequence header replaces them; §7.3.8 expects a shown KEY frame
  /// next, which resets the slots.
  void set_sequence(bool enable_order_hint, uint8_t OrderHintBits) noexcept;

  /// One frame in decode order. Runs: the §5.9.2 reset for a shown KEY frame; the loading process (and the
  /// refresh-all update of a KEY slot) for show_existing_frame; otherwise declared_refs from ref_frame_idx[]
  /// (SYN-AV1-0102 for an empty slot) and the update process with refresh_frame_flags. `prev_qp_base` is used for
  /// show_existing_frame of a frame whose qp_base the caller knows (frames[show_existing_of].qp_base); pass the
  /// callback to receive the frame's diagnostics.
  Av1FrameRecord step(const Av1FrameHeaderValues& h, int64_t decode_index,
                      const std::function<int16_t(int64_t decode_index)>& qp_base_of = {},
                      const IndexDiagnosticCallback& diag = {});

  [[nodiscard]] const syntax::av1::DecoderState& state() const noexcept { return state_; }
  [[nodiscard]] syntax::av1::DecoderState& state() noexcept { return state_; }
  [[nodiscard]] bool enable_order_hint() const noexcept { return enable_order_hint_; }
  [[nodiscard]] uint8_t order_hint_bits() const noexcept { return OrderHintBits_; }

 private:
  Av1FrameRecord step_show_existing(const Av1FrameHeaderValues& h, Av1FrameRecord r,
                                    const std::function<int16_t(int64_t)>& qp_base_of,
                                    const syntax::av1::DiagnosticCallback& state_diag);

  syntax::av1::DecoderState state_;
  bool enable_order_hint_ = false;
  uint8_t OrderHintBits_ = 0;
};

}  // namespace ovt::index

#endif  // OVT_INDEX_AV1_SLOTS_H
