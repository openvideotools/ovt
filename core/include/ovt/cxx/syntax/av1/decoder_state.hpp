// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 reference slot model (chapter 7 §7.3.10.4, WI-1.119): the eight-slot DecoderState, the reference frame
// update process (AV1 §7.20), the reference frame loading process (AV1 §7.21), set_frame_refs() (AV1 §7.8) and
// get_relative_dist() (AV1 §5.9.3), plus the OrderHints / RefFrameSignBias derivation of AV1 §5.9.2 that the
// frame header parser (WI-1.117) and the index (§7.4.4) share. C++ helper header (D22): semi-public, no ABI
// promise, never included from a C header, not installed.
//
// Variables keep their AV1 names (RefValid, RefOrderHint, OrderHintBits, …) so that the code reads against the
// specification tables. CDFs and motion fields are not modelled (§7.3.10.4). Nothing here reads bits: the
// callers hand over parsed values. Conformance findings go to a DiagnosticCallback with the §7.9 codes.

#ifndef OVT_CXX_SYNTAX_AV1_DECODER_STATE_HPP
#define OVT_CXX_SYNTAX_AV1_DECODER_STATE_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ovt::syntax::av1 {

inline constexpr int kNumRefFrames = 8;       // NUM_REF_FRAMES (AV1 §3)
inline constexpr int kRefsPerFrame = 7;       // REFS_PER_FRAME
inline constexpr int kTotalRefsPerFrame = 8;  // TOTAL_REFS_PER_FRAME (INTRA_FRAME + the 7 inter references)
inline constexpr int kPrimaryRefNone = 7;     // PRIMARY_REF_NONE
inline constexpr uint8_t kAllFrames = 0xFF;   // allFrames: refresh_frame_flags of a shown KEY frame
inline constexpr int kMaxSegments = 8;        // MAX_SEGMENTS
inline constexpr int kSegLvlMax = 8;          // SEG_LVL_MAX
inline constexpr int kNumGmParams = 6;        // gm_params[ref][0..5] (AV1 §5.9.24)
inline constexpr int kMaxOrderHintBits = 8;   // order_hint_bits_minus_1 is f(3)

/// ref_frame_idx[0..6] (LAST_FRAME..ALTREF_FRAME): slot numbers 0..7, or -1 for an entry that is not set.
using RefFrameIdx = std::array<int, kRefsPerFrame>;

/// frame_type (AV1 §6.8.2). The values equal ovt.fb.Av1FrameType.
enum class FrameType : uint8_t { Key = 0, Inter = 1, IntraOnly = 2, Switch = 3 };

/// FrameIsIntra of AV1 §5.9.2.
[[nodiscard]] constexpr bool frame_is_intra(FrameType t) noexcept {
  return t == FrameType::Key || t == FrameType::IntraOnly;
}

/// Reference frame names (AV1 §6.10.24); the values equal ovt.fb.Av1RefFrame. refFrame - kLastFrame indexes
/// ref_frame_idx[].
enum RefFrame : int8_t {
  kNoneFrame = -1,
  kIntraFrame = 0,
  kLastFrame = 1,
  kLast2Frame = 2,
  kLast3Frame = 3,
  kGoldenFrame = 4,
  kBwdrefFrame = 5,
  kAltref2Frame = 6,
  kAltrefFrame = 7,
};

/// "INTRA", "LAST", "LAST2", "LAST3", "GOLDEN", "BWDREF", "ALTREF2", "ALTREF" (§7.4.4 RefSlot names); "NONE" for
/// kNoneFrame and any other value.
[[nodiscard]] std::string_view ref_frame_name(int ref_frame) noexcept;

/// "KEY_FRAME", "INTER_FRAME", "INTRA_ONLY_FRAME", "SWITCH_FRAME".
[[nodiscard]] std::string_view frame_type_name(FrameType t) noexcept;

/// film_grain_params() (AV1 §5.9.30 / §6.8.20): the fields the update process saves (SavedFilmGrainParams) and
/// load_grain_params() restores. The frame header parser (WI-1.118) fills it; the arrays are sized by the
/// syntax maxima (14 luma points, 10 chroma points, 24 / 25 AR coefficients).
struct FilmGrainParams {
  bool apply_grain = false;
  uint16_t grain_seed = 0;
  bool update_grain = true;
  uint8_t film_grain_params_ref_idx = 0;
  uint8_t num_y_points = 0;
  std::array<uint8_t, 14> point_y_value{};
  std::array<uint8_t, 14> point_y_scaling{};
  bool chroma_scaling_from_luma = false;
  uint8_t num_cb_points = 0;
  std::array<uint8_t, 10> point_cb_value{};
  std::array<uint8_t, 10> point_cb_scaling{};
  uint8_t num_cr_points = 0;
  std::array<uint8_t, 10> point_cr_value{};
  std::array<uint8_t, 10> point_cr_scaling{};
  uint8_t grain_scaling_minus_8 = 0;
  uint8_t ar_coeff_lag = 0;
  std::array<uint8_t, 24> ar_coeffs_y_plus_128{};
  std::array<uint8_t, 25> ar_coeffs_cb_plus_128{};
  std::array<uint8_t, 25> ar_coeffs_cr_plus_128{};
  uint8_t ar_coeff_shift_minus_6 = 0;
  uint8_t grain_scale_shift = 0;
  uint8_t cb_mult = 0;
  uint8_t cb_luma_mult = 0;
  uint16_t cb_offset = 0;
  uint8_t cr_mult = 0;
  uint8_t cr_luma_mult = 0;
  uint16_t cr_offset = 0;
  bool overlap_flag = false;
  bool clip_to_restricted_range = false;

  friend bool operator==(const FilmGrainParams&, const FilmGrainParams&) = default;
};

using GmParams = std::array<std::array<int32_t, kNumGmParams>, kTotalRefsPerFrame>;  // gm_params[ref][0..5]

/// gm_params with every reference set to the identity warp (AV1 §7.20 setup_past_independence: gm_params[ref][i]
/// = (i % 3 == 2) ? 1 << WARPEDMODEL_PREC_BITS : 0).
[[nodiscard]] GmParams identity_gm_params() noexcept;

/// The frame-level variables of the current frame that the update process copies into a slot and the loading
/// process copies back (AV1 §7.20 / §7.21). The frame header parser fills them; the index reads them.
struct FrameState {
  uint32_t current_frame_id = 0;
  uint32_t UpscaledWidth = 0;
  uint32_t FrameWidth = 0;
  uint32_t FrameHeight = 0;
  uint32_t RenderWidth = 0;
  uint32_t RenderHeight = 0;
  uint32_t MiCols = 0;
  uint32_t MiRows = 0;
  FrameType frame_type = FrameType::Key;
  bool subsampling_x = true;
  bool subsampling_y = true;
  uint8_t BitDepth = 8;
  uint32_t OrderHint = 0;
  bool showable_frame = false;
  std::array<uint32_t, kTotalRefsPerFrame> OrderHints{};  // OrderHints[refFrame], INTRA_FRAME..ALTREF_FRAME
  GmParams gm_params = identity_gm_params();
  std::array<int8_t, kTotalRefsPerFrame> loop_filter_ref_deltas{1, 0, 0, 0, -1, 0, -1, -1};  // §7.20 defaults
  std::array<int8_t, 2> loop_filter_mode_deltas{0, 0};
  std::array<std::array<bool, kSegLvlMax>, kMaxSegments> FeatureEnabled{};
  std::array<std::array<int16_t, kSegLvlMax>, kMaxSegments> FeatureData{};
  FilmGrainParams film_grain_params;

  friend bool operator==(const FrameState&, const FrameState&) = default;
};

/// One of the NUM_REF_FRAMES slots (§7.3.10.4). Field names are the AV1 §7.20 variables with the slot index
/// dropped (RefValid[i] → slots[i].RefValid), as in ovt.fb.Av1RefSlot.
struct RefSlot {
  bool RefValid = false;
  uint32_t RefFrameId = 0;
  uint32_t RefUpscaledWidth = 0;
  uint32_t RefFrameWidth = 0;
  uint32_t RefFrameHeight = 0;
  uint32_t RefRenderWidth = 0;
  uint32_t RefRenderHeight = 0;
  uint32_t RefMiCols = 0;
  uint32_t RefMiRows = 0;
  FrameType RefFrameType = FrameType::Key;
  bool RefSubsamplingX = true;
  bool RefSubsamplingY = true;
  uint8_t RefBitDepth = 8;
  uint32_t RefOrderHint = 0;
  bool RefShowableFrame = false;
  std::array<uint32_t, kTotalRefsPerFrame> SavedOrderHints{};
  GmParams SavedGmParams = identity_gm_params();
  std::array<int8_t, kTotalRefsPerFrame> SavedLoopFilterRefDeltas{1, 0, 0, 0, -1, 0, -1, -1};
  std::array<int8_t, 2> SavedLoopFilterModeDeltas{0, 0};
  std::array<std::array<bool, kSegLvlMax>, kMaxSegments> SavedFeatureEnabled{};
  std::array<std::array<int16_t, kSegLvlMax>, kMaxSegments> SavedFeatureData{};
  FilmGrainParams SavedFilmGrainParams;
  // OVT: FrameId.decode_index of the frame stored in the slot, -1 when the slot is empty (§7.3.10.4).
  int64_t slot_decode_index = -1;

  /// The §7.20 copy for one slot: every Ref*/Saved* field from `cur`, RefValid = 1, slot_decode_index.
  void save(const FrameState& cur, int64_t decode_index) noexcept;
  /// The §7.21 copy back: a FrameState with this slot's Ref*/Saved* values (frame_type = RefFrameType, OrderHint =
  /// RefOrderHint, …). RefValid is not checked here.
  [[nodiscard]] FrameState load() const noexcept;

  friend bool operator==(const RefSlot&, const RefSlot&) = default;
};

/// A conformance finding of the processes below (§7.9 codes SYN-AV1-0012, -0013, -0014). Severity values equal
/// ovt.fb.Severity; every code here is an Error.
struct StateDiagnostic {
  std::string_view code;
  std::string message;
};
using DiagnosticCallback = std::function<void(const StateDiagnostic&)>;  // may be empty: findings are dropped

inline constexpr std::string_view kDiagIntraOnlyRefreshAll = "SYN-AV1-0012";
inline constexpr std::string_view kDiagShowExistingEmptySlot = "SYN-AV1-0013";
inline constexpr std::string_view kDiagShortSignalingOrderHint = "SYN-AV1-0014";

/// The per-stream decoder state of §7.3.10.4: the eight reference slots. Reset on a shown KEY frame (§7.3.8).
struct DecoderState {
  std::array<RefSlot, kNumRefFrames> slots{};

  /// AV1 §5.9.2 at frame_type == KEY_FRAME && show_frame: RefValid[i] = 0 and RefOrderHint[i] = 0 for every slot
  /// (before ref_order_hint[] would be parsed — WI-1.119 notes). slot_decode_index becomes -1. The other saved
  /// values stay: the KEY frame's refresh_frame_flags = allFrames overwrites them.
  void reset_for_shown_key_frame() noexcept;

  /// Every slot back to its construction state (a new stream).
  void clear() noexcept;

  [[nodiscard]] const RefSlot& slot(int i) const noexcept { return slots[static_cast<size_t>(i)]; }
  [[nodiscard]] RefSlot& slot(int i) noexcept { return slots[static_cast<size_t>(i)]; }

  /// slot_decode_index[0..7] as §7.4.4 dpb_after lists them (-1 for empty slots).
  [[nodiscard]] std::array<int64_t, kNumRefFrames> slot_decode_indices() const noexcept;

  friend bool operator==(const DecoderState&, const DecoderState&) = default;
};

/// get_relative_dist(a, b) (AV1 §5.9.3): 0 without enable_order_hint; otherwise the signed distance from b to a
/// modulo 2^OrderHintBits, in [-(2^(OrderHintBits-1)), 2^(OrderHintBits-1) - 1].
[[nodiscard]] int32_t get_relative_dist(uint32_t a, uint32_t b, bool enable_order_hint, uint8_t OrderHintBits) noexcept;

/// Reference frame update process (AV1 §7.20, §7.3.10.4), run after the last tile group of a frame, or for a
/// show_existing_frame of a KEY slot after the loading process with refresh_frame_flags = allFrames. Every slot
/// whose bit is set in refresh_frame_flags receives `cur` and `decode_index`. An INTRA_ONLY frame with
/// refresh_frame_flags == 0xFF is Diagnostic Error SYN-AV1-0012 (AV1 §5.9.2 conformance requirement); the update
/// still runs so that the model follows what a decoder does.
void reference_frame_update(DecoderState& state, const FrameState& cur, uint8_t refresh_frame_flags,
                            int64_t decode_index, const DiagnosticCallback& diag = {});

/// Reference frame loading process (AV1 §7.21, §7.3.10.4) for show_existing_frame == 1 with slot
/// frame_to_show_map_idx. Returns the slot's saved FrameState in `out` and true. An empty slot (RefValid == 0) is
/// Diagnostic Error SYN-AV1-0013: `out` still receives the slot's stale values and false is returned. The caller
/// runs reference_frame_update(state, out, kAllFrames, …) when out.frame_type == FrameType::Key (AV1 §7.21 last
/// step with AV1 §5.9.2 refresh_frame_flags = allFrames).
bool reference_frame_loading(const DecoderState& state, uint8_t frame_to_show_map_idx, FrameState& out,
                             const DiagnosticCallback& diag = {});

/// set_frame_refs() (AV1 §7.8, §7.3.10.4), run when frame_refs_short_signaling == 1 (which requires
/// enable_order_hint). Returns ref_frame_idx[0..6] (LAST_FRAME..ALTREF_FRAME) from last_frame_idx, gold_frame_idx
/// and the slots' RefOrderHint. A LAST or GOLDEN slot whose shifted order hint is not before the current frame
/// is Diagnostic Error SYN-AV1-0014 (one per offending index); the derivation continues as the spec writes it.
[[nodiscard]] RefFrameIdx set_frame_refs(const DecoderState& state, uint32_t OrderHint, uint8_t last_frame_idx,
                                         uint8_t gold_frame_idx, bool enable_order_hint, uint8_t OrderHintBits,
                                         const DiagnosticCallback& diag = {});

/// OrderHints[refFrame] and RefFrameSignBias[refFrame] of AV1 §5.9.2 (the loop after ref_frame_idx[] is known):
/// for refFrame LAST_FRAME..ALTREF_FRAME, hint = RefOrderHint[ref_frame_idx[refFrame - LAST_FRAME]], OrderHints
/// [refFrame] = hint, RefFrameSignBias[refFrame] = get_relative_dist(hint, OrderHint) > 0 when enable_order_hint,
/// else 0. Entry 0 (INTRA_FRAME) stays 0 / false.
struct RefOrderHints {
  std::array<uint32_t, kTotalRefsPerFrame> OrderHints{};
  std::array<bool, kTotalRefsPerFrame> RefFrameSignBias{};
};
[[nodiscard]] RefOrderHints compute_order_hints(const DecoderState& state, const RefFrameIdx& ref_frame_idx,
                                                uint32_t OrderHint, bool enable_order_hint,
                                                uint8_t OrderHintBits) noexcept;

}  // namespace ovt::syntax::av1

#endif  // OVT_CXX_SYNTAX_AV1_DECODER_STATE_HPP
