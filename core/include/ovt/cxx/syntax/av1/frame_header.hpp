// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 uncompressed_header() (AV1 §5.9.2) and its sub-structures: chapter 7 §7.3.6 rows 11–19, WI-1.117 (part 1:
// up to and including delta_lf_params()); rows 20–29 follow in WI-1.118. Rows 12–15 (frame size, render size,
// frame_size_with_refs, tile_info) and rows 16–19 (quantization, segmentation, delta q / lf, the lossless
// derivations) came in WI-1.117a/b; the FrameHeader struct and parse_frame_header() for row 11 in WI-1.117c. C++
// helper header (D22): semi-public, no ABI promise, never included from a C header, not installed.
//
// The typed structs hold every syntax element under its spec name and every derived variable (§7.3.7); the
// SyntaxNode tree is for display. Fields the syntax table infers instead of reading carry the inferred value.
// The parsers read bits only; the reference slot processes of AV1 §7.20 / §7.21 live in decoder_state.hpp and
// run in the index (§7.3.10.4). Members of each struct are grouped by size, largest first (clang-analyzer
// optin.performance.Padding); the comments give the syntax order.

#ifndef OVT_CXX_SYNTAX_AV1_FRAME_HEADER_HPP
#define OVT_CXX_SYNTAX_AV1_FRAME_HEADER_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::av1 {

// AV1 §3 constants used by rows 12–19.
inline constexpr uint32_t kMiSize = 4;                   // MI_SIZE: luma samples per mode-info unit
inline constexpr uint32_t kMaxTileWidth = 4096;          // MAX_TILE_WIDTH
inline constexpr uint32_t kMaxTileArea = 4096U * 2304U;  // MAX_TILE_AREA
inline constexpr uint32_t kMaxTileCols = 64;             // MAX_TILE_COLS
inline constexpr uint32_t kMaxTileRows = 64;             // MAX_TILE_ROWS
inline constexpr uint8_t kSuperresNum = 8;               // SUPERRES_NUM
inline constexpr uint8_t kSuperresDenomMin = 9;          // SUPERRES_DENOM_MIN
inline constexpr uint8_t kSuperresDenomBits = 3;         // SUPERRES_DENOM_BITS
inline constexpr uint8_t kMaxLoopFilter = 63;            // MAX_LOOP_FILTER
inline constexpr uint8_t kSegLvlAltQ = 0;                // SEG_LVL_ALT_Q
inline constexpr uint8_t kSegLvlRefFrame = 5;            // SEG_LVL_REF_FRAME
inline constexpr uint8_t kSegLvlSkip = 6;                // SEG_LVL_SKIP
inline constexpr uint8_t kSegLvlGlobalMv = 7;            // SEG_LVL_GLOBALMV
inline constexpr uint8_t kQmLevelLossless = 15;          // SegQMLevel of a lossless segment (AV1 §5.9.2)

// Segmentation_Feature_Bits / _Signed / _Max (AV1 §5.9.14).
inline constexpr std::array<uint8_t, kSegLvlMax> kSegmentationFeatureBits{8, 6, 6, 6, 6, 3, 0, 0};
inline constexpr std::array<bool, kSegLvlMax> kSegmentationFeatureSigned{true, true,  true,  true,
                                                                         true, false, false, false};
inline constexpr std::array<int16_t, kSegLvlMax> kSegmentationFeatureMax{
    255, kMaxLoopFilter, kMaxLoopFilter, kMaxLoopFilter, kMaxLoopFilter, 7, 0, 0};

/// "SEG_LVL_ALT_Q", "SEG_LVL_ALT_LF_Y_V", "SEG_LVL_ALT_LF_Y_H", "SEG_LVL_ALT_LF_U", "SEG_LVL_ALT_LF_V",
/// "SEG_LVL_REF_FRAME", "SEG_LVL_SKIP", "SEG_LVL_GLOBALMV" (§7.3.6 row 17 feature names); "" otherwise.
[[nodiscard]] std::string_view segmentation_feature_name(int feature) noexcept;

/// interpolation_filter (AV1 §6.8.9). The values equal ovt.fb.Av1InterpFilter.
enum class InterpolationFilter : uint8_t {
  EightTap = 0,
  EightTapSmooth = 1,
  EightTapSharp = 2,
  Bilinear = 3,
  Switchable = 4,
};
/// "EIGHTTAP", "EIGHTTAP_SMOOTH", "EIGHTTAP_SHARP", "BILINEAR", "SWITCHABLE".
[[nodiscard]] std::string_view interpolation_filter_name(InterpolationFilter f) noexcept;

/// Diagnostic codes of rows 11–19 (§7.9). SYN-AV1-0015 (registered through #513) is the Error for frame-header
/// values that violate a conformance requirement without being a reserved value (see frame_size.cpp /
/// tile_info.cpp); SYN-AV1-0011 is the Info parse_frame_header() raises for a ref_order_hint mismatch.
inline constexpr std::string_view kDiagRefOrderHintMismatch = "SYN-AV1-0011";  // Info
inline constexpr std::string_view kDiagFrameHeaderRange = "SYN-AV1-0015";      // Error

// ---- row 12: frame_size(), superres_params(), compute_image_size() ---------------------------------------------

struct FrameSize {
  BitSpan span{0, 0};
  // frame_width_minus_1 f(n) / frame_height_minus_1 f(n) when frame_size_override_flag, else the sequence maxima.
  uint32_t frame_width_minus_1 = 0;
  uint32_t frame_height_minus_1 = 0;
  // Derived (AV1 §5.9.5 / §5.9.8 / §5.9.9)
  uint32_t UpscaledWidth = 0;  // frame_width_minus_1 + 1 before superres downscaling
  uint32_t FrameWidth = 0;     // (UpscaledWidth * SUPERRES_NUM + SuperresDenom / 2) / SuperresDenom
  uint32_t FrameHeight = 0;
  uint32_t MiCols = 0;  // 2 * ((FrameWidth + 7) >> 3)
  uint32_t MiRows = 0;
  // superres_params()
  uint8_t coded_denom = 0;
  uint8_t SuperresDenom = kSuperresNum;  // coded_denom + SUPERRES_DENOM_MIN, or SUPERRES_NUM without superres
  bool use_superres = false;
  bool size_from_sequence_header = false;  // derived: frame_size_override_flag == 0 (nothing read for the size)
};

/// frame_size() with superres_params() and compute_image_size() at the current position (node "frame_size").
/// `frame_size_override_flag` selects the two f(n) reads (n = frame_width_bits_minus_1 + 1 …) or the sequence
/// maxima. A frame larger than max_frame_width/height is Diagnostic Error SYN-AV1-0015; the values stand.
[[nodiscard]] FrameSize parse_frame_size(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                         bool frame_size_override_flag);

/// superres_params() (AV1 §5.9.8) inside the current struct: use_superres (read only with enable_superres),
/// coded_denom, SuperresDenom; sets UpscaledWidth from FrameWidth and downscales FrameWidth.
void parse_superres_params(SyntaxBuilder& b, const SequenceHeader& seq, FrameSize& fs);

/// compute_image_size() (AV1 §5.9.9): MiCols / MiRows from FrameWidth / FrameHeight.
void compute_image_size(FrameSize& fs) noexcept;

// ---- row 13: render_size() ------------------------------------------------------------------------------------

struct RenderSize {
  BitSpan span{0, 0};
  uint32_t render_width_minus_1 = 0;  // f(16), only when render_and_frame_size_different
  uint32_t render_height_minus_1 = 0;
  // Derived
  uint32_t RenderWidth = 0;  // render_width_minus_1 + 1, or UpscaledWidth
  uint32_t RenderHeight = 0;
  bool render_and_frame_size_different = false;
};

/// render_size() at the current position (node "render_size"); the defaults come from `fs`.
[[nodiscard]] RenderSize parse_render_size(BitReader& br, SyntaxBuilder& b, const FrameSize& fs);

// ---- row 14: frame_size_with_refs() ---------------------------------------------------------------------------

struct FrameSizeWithRefs {
  BitSpan span{0, 0};
  std::array<bool, kRefsPerFrame> found_ref{};  // found_ref f(1) for i = 0 .. found_ref_index (the rest unread)
  int8_t found_ref_index = -1;                  // the i whose found_ref was 1, or -1 (frame_size() was read)
};

/// frame_size_with_refs() (AV1 §5.9.7) at the current position (node "frame_size_with_refs"). With a found
/// reference the sizes are copied from slot ref_frame_idx[i] of `state` (RefUpscaledWidth, RefFrameHeight,
/// RefRenderWidth/Height), then superres_params() and compute_image_size() run; otherwise frame_size() and
/// render_size() are read. `fs` and `rs` receive the result. A found reference to an empty slot (RefValid == 0)
/// is Diagnostic Error SYN-AV1-0015 and the stale slot values are used, as a decoder would.
[[nodiscard]] FrameSizeWithRefs parse_frame_size_with_refs(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                                           const DecoderState& state, const RefFrameIdx& ref_frame_idx,
                                                           bool frame_size_override_flag, FrameSize& fs,
                                                           RenderSize& rs);

// ---- row 15: tile_info() --------------------------------------------------------------------------------------

/// A tile rectangle in luma samples (MiColStarts[i] * MI_SIZE …; the last column / row ends at MiCols * MI_SIZE,
/// which may exceed FrameWidth by up to 7 samples).
struct TileRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct TileInfo {
  std::vector<uint32_t> MiColStarts;  // TileCols + 1 entries, the last one MiCols
  std::vector<uint32_t> MiRowStarts;  // TileRows + 1 entries, the last one MiRows
  // width_in_sbs_minus_1 ns(maxWidth) / height_in_sbs_minus_1 ns(maxHeight): one per tile column / row when
  // uniform_tile_spacing_flag == 0; empty otherwise.
  std::vector<uint32_t> width_in_sbs_minus_1;
  std::vector<uint32_t> height_in_sbs_minus_1;
  BitSpan span{0, 0};
  // Derived superblock geometry (AV1 §5.9.15 locals, kept for the tests and the tile group parser)
  uint32_t sbCols = 0;
  uint32_t sbRows = 0;
  uint32_t minLog2TileCols = 0;
  uint32_t maxLog2TileCols = 0;
  uint32_t maxLog2TileRows = 0;
  uint32_t minLog2Tiles = 0;
  uint32_t widestTileSb = 0;  // non-uniform spacing only
  // Derived tile counts
  uint32_t TileCols = 0;
  uint32_t TileRows = 0;
  uint32_t NumTiles = 0;                // TileCols * TileRows
  uint32_t context_update_tile_id = 0;  // f(TileRowsLog2 + TileColsLog2), or 0
  uint8_t TileColsLog2 = 0;
  uint8_t TileRowsLog2 = 0;
  uint8_t tile_size_bytes_minus_1 = 3;  // f(2), only when TileColsLog2 > 0 || TileRowsLog2 > 0 (3 otherwise)
  uint8_t TileSizeBytes = 4;            // tile_size_minus_1 width in tile_group_obu(); 4 when not read (AV1 §5.9.15)
  uint8_t sbShift = 4;                  // 5 with 128x128 superblocks
  bool uniform_tile_spacing_flag = false;

  /// The rectangle of tile (tileRow, tileCol) in luma samples; {0,0,0,0} when out of range.
  [[nodiscard]] TileRect tile_rect(uint32_t tileRow, uint32_t tileCol) const noexcept;
};

/// tile_log2(blkSize, target) (AV1 §5.9.15): the smallest k with (blkSize << k) >= target.
[[nodiscard]] uint32_t tile_log2(uint32_t blkSize, uint32_t target) noexcept;

/// tile_info() (AV1 §5.9.15) at the current position (node "tile_info") for a frame of MiCols × MiRows mode-info
/// units with the superblock size of `seq`. More than MAX_TILE_COLS columns or MAX_TILE_ROWS rows from non-uniform
/// spacing is Diagnostic Error SYN-AV1-0015 (a conformance requirement of AV1 §5.9.15); the starts are kept.
[[nodiscard]] TileInfo parse_tile_info(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, uint32_t MiCols,
                                       uint32_t MiRows);

// ---- row 16: quantization_params() ----------------------------------------------------------------------------

struct QuantizationParams {
  BitSpan span{0, 0};
  // read_delta_q() results (AV1 §5.9.13), in syntax order after base_q_idx
  int32_t DeltaQYDc = 0;
  int32_t DeltaQUDc = 0;
  int32_t DeltaQUAc = 0;
  int32_t DeltaQVDc = 0;  // copies of U unless diff_uv_delta
  int32_t DeltaQVAc = 0;
  uint8_t base_q_idx = 0;
  uint8_t qm_y = 0;  // f(4), only when using_qmatrix
  uint8_t qm_u = 0;
  uint8_t qm_v = 0;            // f(4) when separate_uv_delta_q, else qm_u
  bool diff_uv_delta = false;  // f(1) when NumPlanes > 1 && separate_uv_delta_q
  bool using_qmatrix = false;
};

/// quantization_params() (AV1 §5.9.12) at the current position (node "quantization_params"); `cc` supplies
/// NumPlanes and separate_uv_delta_q.
[[nodiscard]] QuantizationParams parse_quantization_params(BitReader& br, SyntaxBuilder& b, const ColorConfig& cc);

// ---- row 17: segmentation_params() ----------------------------------------------------------------------------

using FeatureEnabledTable = std::array<std::array<bool, kSegLvlMax>, kMaxSegments>;  // FeatureEnabled[i][j]
using FeatureDataTable = std::array<std::array<int16_t, kSegLvlMax>, kMaxSegments>;  // FeatureData[i][j] (clipped)

struct SegmentationParams {
  FeatureDataTable FeatureData{};
  FeatureEnabledTable FeatureEnabled{};
  BitSpan span{0, 0};
  uint8_t LastActiveSegId = 0;  // derived: highest i with any FeatureEnabled[i][j]
  bool segmentation_enabled = false;
  bool segmentation_update_map = false;  // inferred 1 / 0 / 1 when primary_ref_frame == PRIMARY_REF_NONE
  bool segmentation_temporal_update = false;
  bool segmentation_update_data = false;
  bool SegIdPreSkip = false;  // derived: a feature j >= SEG_LVL_REF_FRAME is enabled
};

/// segmentation_params() (AV1 §5.9.14) at the current position (node "segmentation_params"). When
/// segmentation_enabled && !segmentation_update_data the feature tables keep the values that load_previous() /
/// setup_past_independence() established (`prev_enabled`, `prev_data`: the FrameState of the primary reference, or
/// zeros); without segmentation every entry is 0.
[[nodiscard]] SegmentationParams parse_segmentation_params(BitReader& br, SyntaxBuilder& b, uint8_t primary_ref_frame,
                                                           const FeatureEnabledTable& prev_enabled,
                                                           const FeatureDataTable& prev_data);

// ---- rows 18 and 19: delta_q_params(), delta_lf_params() -----------------------------------------------------

struct DeltaQParams {
  BitSpan span{0, 0};
  uint8_t delta_q_res = 0;       // f(2), only when delta_q_present
  bool delta_q_present = false;  // f(1), only when base_q_idx > 0
};

/// delta_q_params() (AV1 §5.9.17) at the current position (node "delta_q_params").
[[nodiscard]] DeltaQParams parse_delta_q_params(BitReader& br, SyntaxBuilder& b, uint8_t base_q_idx);

struct DeltaLfParams {
  BitSpan span{0, 0};
  uint8_t delta_lf_res = 0;       // f(2), only when delta_lf_present
  bool delta_lf_present = false;  // f(1), only when delta_q_present && !allow_intrabc
  bool delta_lf_multi = false;
};

/// delta_lf_params() (AV1 §5.9.18) at the current position (node "delta_lf_params").
[[nodiscard]] DeltaLfParams parse_delta_lf_params(BitReader& br, SyntaxBuilder& b, bool delta_q_present,
                                                  bool allow_intrabc);

// ---- lossless derivations (row 11, after delta_lf_params) -----------------------------------------------------

struct Lossless {
  std::array<std::array<uint8_t, kMaxSegments>, 3> SegQMLevel{};  // [plane][segmentId], only when using_qmatrix
  std::array<bool, kMaxSegments> LosslessArray{};
  bool CodedLossless = false;
  bool AllLossless = false;  // CodedLossless && FrameWidth == UpscaledWidth
};

/// get_qindex(ignoreDeltaQ, segmentId) (AV1 §7.12.2): base_q_idx (or, when !ignoreDeltaQ && delta_q_present,
/// the caller's CurrentQIndex) plus the clipped SEG_LVL_ALT_Q delta of the segment. The header derivations call it
/// with ignoreDeltaQ = true; chapter 7.6 passes the tile's CurrentQIndex and the frame's delta_q_present.
[[nodiscard]] int32_t get_qindex(bool ignoreDeltaQ, int segmentId, const QuantizationParams& q,
                                 const SegmentationParams& seg, bool delta_q_present, int32_t CurrentQIndex) noexcept;

/// CodedLossless, LosslessArray[], SegQMLevel[][] and AllLossless of AV1 §5.9.2 (§7.3.6 row 11).
[[nodiscard]] Lossless compute_lossless(const QuantizationParams& q, const SegmentationParams& seg,
                                        bool frame_width_equals_upscaled_width) noexcept;

// ---- row 11: uncompressed_header() ----------------------------------------------------------------------------

/// temporal_point_info() (AV1 §5.9.31), read when decoder_model_info_present_flag && !equal_picture_interval.
struct TemporalPointInfo {
  BitSpan span{0, 0};
  uint32_t frame_presentation_time = 0;  // f(frame_presentation_time_length_minus_1 + 1)
  bool present = false;
};

/// buffer_removal_time[opNum] (AV1 §5.9.2), one entry per operating point whose decoder model applies to the
/// OBU's temporal_id / spatial_id.
struct BufferRemovalTime {
  uint32_t opNum = 0;
  uint32_t buffer_removal_time = 0;  // f(buffer_removal_time_length_minus_1 + 1)
};

/// §7.3.6 row 11: uncompressed_header(), part 1 (WI-1.117): everything up to and including delta_lf_params() and
/// the lossless derivations. The rows 20–29 fields (loop filter, CDEF, loop restoration, tx mode, reference mode,
/// skip mode, global motion, film grain) are WI-1.118; until then `part1_only` is true and `state` carries the
/// defaults of setup_past_independence() / the values of load_previous() for them.
struct FrameHeader {
  // The §7.20 variables of this frame (sizes, frame_type, OrderHint, OrderHints, feature tables, loop filter
  // deltas, gm_params, film grain): what the reference frame update process saves. For show_existing_frame it is
  // the loaded slot (AV1 §7.21) as the slot model will restore it.
  FrameState state;
  TileInfo tile_info;
  SegmentationParams segmentation_params;
  std::vector<BufferRemovalTime> buffer_removal_time;  // empty unless buffer_removal_time_present_flag
  QuantizationParams quantization_params;
  Lossless lossless;
  FrameSize frame_size;
  RenderSize render_size;
  FrameSizeWithRefs frame_size_with_refs;  // valid when frame_size_with_refs_present
  TemporalPointInfo temporal_point_info;
  DeltaQParams delta_q_params;
  DeltaLfParams delta_lf_params;
  BitSpan span{0, 0};
  // ref_order_hint[i] f(OrderHintBits), only with error_resilient_mode && enable_order_hint (ref_order_hint_present)
  std::array<uint32_t, kNumRefFrames> ref_order_hint{};
  // ref_frame_idx[0..6] (LAST_FRAME..ALTREF_FRAME): read, or from set_frame_refs(); -1 for an intra frame
  RefFrameIdx ref_frame_idx{-1, -1, -1, -1, -1, -1, -1};
  // delta_frame_id_minus_1[i] f(delta_frame_id_length_minus_2 + 2) and the derived expectedFrameId[i], only with
  // frame_id_numbers_present_flag
  std::array<uint32_t, kRefsPerFrame> delta_frame_id_minus_1{};
  std::array<uint32_t, kRefsPerFrame> expectedFrameId{};
  std::array<bool, kTotalRefsPerFrame> RefFrameSignBias{};  // derived with state.OrderHints (AV1 §5.9.2)
  // Syntax order: show_existing_frame, frame_to_show_map_idx, temporal_point_info, display_frame_id | frame_type,
  // show_frame, temporal_point_info, showable_frame, error_resilient_mode, disable_cdf_update,
  // allow_screen_content_tools, force_integer_mv, current_frame_id, frame_size_override_flag, order_hint,
  // primary_ref_frame, buffer_removal_time_present_flag, buffer_removal_time[], refresh_frame_flags,
  // ref_order_hint[], frame_size/render_size/allow_intrabc | frame_refs_short_signaling, last_frame_idx,
  // gold_frame_idx, ref_frame_idx[], delta_frame_id_minus_1[], frame_size_with_refs | frame_size, render_size,
  // allow_high_precision_mv, is_filter_switchable, interpolation_filter, is_motion_mode_switchable,
  // use_ref_frame_mvs; disable_frame_end_update_cdf; tile_info, quantization_params, segmentation_params,
  // delta_q_params, delta_lf_params.
  uint32_t display_frame_id = 0;
  uint32_t current_frame_id = 0;  // also state.current_frame_id
  uint32_t order_hint = 0;        // f(OrderHintBits); OrderHint = state.OrderHint
  uint8_t frame_to_show_map_idx = 0;
  uint8_t primary_ref_frame = kPrimaryRefNone;
  uint8_t refresh_frame_flags = 0;
  uint8_t last_frame_idx = 0;  // f(3), only with frame_refs_short_signaling
  uint8_t gold_frame_idx = 0;
  InterpolationFilter interpolation_filter = InterpolationFilter::EightTap;
  FrameType frame_type = FrameType::Key;  // also state.frame_type; RefFrameType[slot] for show_existing_frame
  bool show_existing_frame = false;
  bool FrameIsIntra = true;
  bool show_frame = true;
  bool showable_frame = false;  // also state.showable_frame
  bool error_resilient_mode = true;
  bool disable_cdf_update = false;
  bool allow_screen_content_tools = false;
  bool force_integer_mv = true;
  bool frame_size_override_flag = false;
  bool buffer_removal_time_present_flag = false;
  bool allow_high_precision_mv = false;
  bool use_ref_frame_mvs = false;
  bool allow_intrabc = false;
  bool frame_refs_short_signaling = false;
  bool is_filter_switchable = false;
  bool is_motion_mode_switchable = false;
  bool disable_frame_end_update_cdf = true;
  bool ref_order_hint_present = false;
  bool frame_size_with_refs_present = false;
  // Derived summary for FrameInfo (§7.3.6 row 11 "Derived summary"): is_rap = KEY_FRAME && show_frame (a
  // show_existing_frame of a KEY slot is is_rap in the slot model, §7.4.3); poc = state.OrderHint; qp_base =
  // quantization_params.base_q_idx; coded_size = FrameWidth × FrameHeight; crop = RenderWidth × RenderHeight.
  bool is_rap = false;
  bool part1_only = true;  // rows 20–29 not parsed (WI-1.118)
};

/// §7.3.7: uncompressed_header() at the current position (node "uncompressed_header") for the OBU `obu`
/// (temporal_id / spatial_id decide which buffer_removal_time[opNum] are present). Reads the §5.9.2 syntax up to
/// delta_lf_params() and derives the lossless flags; `state` is consulted for show_existing_frame, ref_order_hint[],
/// frame_size_with_refs(), set_frame_refs() and load_previous(), and changed only where §5.9.2 changes it inside
/// the header (shown-KEY reset, mark_ref_frames(), RefValid cleared on a ref_order_hint mismatch, SYN-AV1-0011).
/// The §7.20 / §7.21 processes stay with the slot model (index). With `is_redundant` (OBU_REDUNDANT_FRAME_HEADER,
/// frame_header_copy()) the header is parsed against a copy of `state`. Throws ParseError (BR-0001) on a truncated
/// header; conformance findings are diagnostics on the tree.
[[nodiscard]] FrameHeader parse_frame_header(BitReader& br, SyntaxBuilder& b, const ObuHeader& obu,
                                             const SequenceHeader& seq, DecoderState& state, bool is_redundant);
/// The §7.3.7 signature: the same for an OBU without extension header (temporal_id = spatial_id = 0;
/// SPEC-ASSUMPTION(#513)).
[[nodiscard]] FrameHeader parse_frame_header(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                             DecoderState& state, bool is_redundant);

/// mark_ref_frames(idLen) (AV1 §5.9.4): clears RefValid of every slot whose RefFrameId is too far from
/// current_frame_id (diffLen = delta_frame_id_length_minus_2 + 2). A shown KEY frame has already cleared them.
void mark_ref_frames(DecoderState& state, const SequenceHeader& seq, uint32_t current_frame_id, FrameType frame_type,
                     bool show_frame) noexcept;

// ==== WI-1.118: §7.3.6 rows 20–27 ===============================================================================
// The parsers below take the frame-level inputs uncompressed_header() has derived by then (CodedLossless,
// AllLossless, allow_intrabc, FrameIsIntra, ref_frame_idx[], OrderHint, the primary reference's saved values) as
// arguments, so that each one can be tested and fuzzed on its own; parse_frame_header() (row 11) calls them in
// syntax order.

// AV1 §3 constants used by rows 20–27.
inline constexpr uint16_t kRestorationTileSizeMax = 256;  // RESTORATION_TILESIZE_MAX
inline constexpr uint8_t kWarpedModelPrecBits = 16;       // WARPEDMODEL_PREC_BITS
inline constexpr uint8_t kGmAbsTransBits = 12;            // GM_ABS_TRANS_BITS
inline constexpr uint8_t kGmAbsTransOnlyBits = 9;         // GM_ABS_TRANS_ONLY_BITS
inline constexpr uint8_t kGmAbsAlphaBits = 12;            // GM_ABS_ALPHA_BITS
inline constexpr uint8_t kGmAlphaPrecBits = 15;           // GM_ALPHA_PREC_BITS
inline constexpr uint8_t kGmTransPrecBits = 6;            // GM_TRANS_PREC_BITS
inline constexpr uint8_t kGmTransOnlyPrecBits = 3;        // GM_TRANS_ONLY_PREC_BITS
inline constexpr uint8_t kMaxNumYPoints = 14;             // num_y_points <= 14 (AV1 §6.8.20 conformance)
inline constexpr uint8_t kMaxNumCbCrPoints = 10;          // num_cb_points / num_cr_points <= 10 (AV1 §6.8.20)

/// The loop_filter_ref_deltas of setup_past_independence() and of the lossless / intrabc branch of
/// loop_filter_params(): INTRA_FRAME 1, LAST..LAST3 0, GOLDEN -1, BWDREF 0, ALTREF2 -1, ALTREF -1.
inline constexpr std::array<int8_t, kTotalRefsPerFrame> kDefaultLoopFilterRefDeltas{1, 0, 0, 0, -1, 0, -1, -1};

// ---- row 20: loop_filter_params() ---------------------------------------------------------------------------

struct LoopFilterParams {
  BitSpan span{0, 0};
  // loop_filter_ref_deltas[i] su(1+6) / loop_filter_mode_deltas[i] su(1+6) where update_*_delta is set; the
  // other entries keep the values of the primary reference frame (load_previous()) or setup_past_independence().
  std::array<int8_t, kTotalRefsPerFrame> loop_filter_ref_deltas = kDefaultLoopFilterRefDeltas;
  std::array<int8_t, 2> loop_filter_mode_deltas{0, 0};
  std::array<bool, kTotalRefsPerFrame> update_ref_delta{};
  std::array<bool, 2> update_mode_delta{};
  // loop_filter_level[0] / [1] f(6); [2] / [3] f(6) only when NumPlanes > 1 and [0] or [1] is non-zero (else 0)
  std::array<uint8_t, 4> loop_filter_level{};
  uint8_t loop_filter_sharpness = 0;  // f(3)
  bool loop_filter_delta_enabled = false;
  bool loop_filter_delta_update = false;
  bool coded = false;  // derived: false for the CodedLossless || allow_intrabc branch (nothing read)
};

/// loop_filter_params() (AV1 §5.9.11) at the current position (node "loop_filter_params"). `prev_ref_deltas` /
/// `prev_mode_deltas` are the deltas in force before the call: FrameState::loop_filter_ref_deltas / _mode_deltas
/// of the primary reference, or the setup_past_independence() defaults.
[[nodiscard]] LoopFilterParams parse_loop_filter_params(BitReader& br, SyntaxBuilder& b, const ColorConfig& cc,
                                                        bool CodedLossless, bool allow_intrabc,
                                                        const std::array<int8_t, kTotalRefsPerFrame>& prev_ref_deltas,
                                                        const std::array<int8_t, 2>& prev_mode_deltas);

// ---- row 21: cdef_params() ----------------------------------------------------------------------------------

struct CdefParams {
  BitSpan span{0, 0};
  // One entry per CDEF strength index i < (1 << cdef_bits); secondary strengths with the 3 → 4 mapping applied.
  std::array<uint8_t, 8> cdef_y_pri_strength{};
  std::array<uint8_t, 8> cdef_y_sec_strength{};
  std::array<uint8_t, 8> cdef_uv_pri_strength{};  // f(4), only when NumPlanes > 1
  std::array<uint8_t, 8> cdef_uv_sec_strength{};
  uint8_t cdef_damping_minus_3 = 0;  // f(2)
  uint8_t CdefDamping = 3;           // derived: cdef_damping_minus_3 + 3
  uint8_t cdef_bits = 0;             // f(2)
  bool coded = false;                // derived: false for the CodedLossless || allow_intrabc || !enable_cdef branch
};

/// cdef_params() (AV1 §5.9.19) at the current position (node "cdef_params"); `seq` supplies enable_cdef and
/// NumPlanes.
[[nodiscard]] CdefParams parse_cdef_params(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                           bool CodedLossless, bool allow_intrabc);

// ---- row 22: lr_params() ------------------------------------------------------------------------------------

/// FrameRestorationType (AV1 §6.10.15). The values equal ovt.fb.Av1RestorationType.
enum class RestorationType : uint8_t { None = 0, Wiener = 1, Sgrproj = 2, Switchable = 3 };
/// Remap_Lr_Type[lr_type] (AV1 §5.9.20).
inline constexpr std::array<RestorationType, 4> kRemapLrType{RestorationType::None, RestorationType::Switchable,
                                                             RestorationType::Wiener, RestorationType::Sgrproj};
/// "RESTORE_NONE", "RESTORE_WIENER", "RESTORE_SGRPROJ", "RESTORE_SWITCHABLE".
[[nodiscard]] std::string_view restoration_type_name(RestorationType t) noexcept;

/// count_units_in_frame(unitSize, frameSize) (AV1 §5.11.57): Max((frameSize + (unitSize >> 1)) / unitSize, 1).
[[nodiscard]] uint32_t count_units_in_frame(uint32_t unitSize, uint32_t frameSize) noexcept;

struct LrParams {
  BitSpan span{0, 0};
  std::array<RestorationType, 3> FrameRestorationType{};  // Remap_Lr_Type[lr_type], RESTORE_NONE when not read
  std::array<uint8_t, 3> lr_type{};                       // f(2) per plane < NumPlanes
  // LoopRestorationSize[plane] = RESTORATION_TILESIZE_MAX >> (2 - lr_unit_shift), chroma >> lr_uv_shift; only
  // meaningful with UsesLr (kept at RESTORATION_TILESIZE_MAX otherwise).
  std::array<uint16_t, 3> LoopRestorationSize{kRestorationTileSizeMax, kRestorationTileSizeMax,
                                              kRestorationTileSizeMax};
  // Derived LR unit grid per plane (AV1 §5.11.57: unitRows / unitCols from count_units_in_frame() on the
  // upscaled plane size); 0 for planes with RESTORE_NONE or without UsesLr.
  std::array<uint32_t, 3> LrUnitRows{};
  std::array<uint32_t, 3> LrUnitCols{};
  uint8_t lr_unit_shift = 0;  // after the increment / lr_unit_extra_shift of §5.9.20 (0..2)
  bool lr_unit_extra_shift = false;
  uint8_t lr_uv_shift = 0;  // f(1) when subsampling_x && subsampling_y && usesChromaLr, else 0
  bool UsesLr = false;
  bool usesChromaLr = false;
  bool coded = false;  // derived: false for the AllLossless || allow_intrabc || !enable_restoration branch
};

/// lr_params() (AV1 §5.9.20) at the current position (node "lr_params"). `UpscaledWidth` / `FrameHeight` size the
/// derived unit grid.
[[nodiscard]] LrParams parse_lr_params(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, bool AllLossless,
                                       bool allow_intrabc, uint32_t UpscaledWidth, uint32_t FrameHeight);

// ---- row 23: read_tx_mode() ---------------------------------------------------------------------------------

/// TxMode (AV1 §6.8.21): the values of ovt.fb.Av1FilterParams.TxMode.
enum class TxModeType : uint8_t { Only4x4 = 0, Largest = 1, Select = 2 };
/// "ONLY_4X4", "TX_MODE_LARGEST", "TX_MODE_SELECT".
[[nodiscard]] std::string_view tx_mode_name(TxModeType m) noexcept;

struct TxModeParams {
  BitSpan span{0, 0};
  TxModeType TxMode = TxModeType::Only4x4;
  bool tx_mode_select = false;  // f(1), only without CodedLossless
};

/// read_tx_mode() (AV1 §5.9.21) at the current position (node "tx_mode").
[[nodiscard]] TxModeParams parse_tx_mode(BitReader& br, SyntaxBuilder& b, bool CodedLossless);

// ---- rows 24 and 25: frame_reference_mode(), skip_mode_params() ---------------------------------------------

/// frame_reference_mode() (AV1 §5.9.23) at the current position (node "frame_reference_mode"): reference_select
/// f(1), or 0 for an intra frame.
[[nodiscard]] bool parse_frame_reference_mode(BitReader& br, SyntaxBuilder& b, bool FrameIsIntra);

struct SkipModeParams {
  BitSpan span{0, 0};
  // SkipModeFrame[0..1] (LAST_FRAME..ALTREF_FRAME), kNoneFrame when !skipModeAllowed
  std::array<int8_t, 2> SkipModeFrame{kNoneFrame, kNoneFrame};
  // §5.9.22 locals, kept for display and the tests: the chosen reference indices (0..6) or -1
  int8_t forwardIdx = -1;
  int8_t backwardIdx = -1;
  int8_t secondForwardIdx = -1;
  bool skipModeAllowed = false;
  bool skip_mode_present = false;  // f(1), only when skipModeAllowed
};

/// skip_mode_params() (AV1 §5.9.22) at the current position (node "skip_mode_params"). RefOrderHint[] comes from
/// the slots of `state` named by `ref_frame_idx`; an unset entry (-1) counts as order hint 0.
[[nodiscard]] SkipModeParams parse_skip_mode_params(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                                    const DecoderState& state, const RefFrameIdx& ref_frame_idx,
                                                    uint32_t OrderHint, bool FrameIsIntra, bool reference_select);

// ---- row 26: global_motion_params() -------------------------------------------------------------------------

/// GmType (AV1 §6.8.17). The values equal ovt.fb.Av1GmType.
enum class GlobalMotionType : uint8_t { Identity = 0, Translation = 1, RotZoom = 2, Affine = 3 };
/// "IDENTITY", "TRANSLATION", "ROTZOOM", "AFFINE".
[[nodiscard]] std::string_view gm_type_name(GlobalMotionType t) noexcept;

struct GlobalMotionParams {
  GmParams gm_params = identity_gm_params();                  // gm_params[ref][0..5], entry 0 (INTRA_FRAME) unused
  std::array<GlobalMotionType, kTotalRefsPerFrame> GmType{};  // GmType[ref], IDENTITY when !is_global
  BitSpan span{0, 0};
  std::array<bool, kTotalRefsPerFrame> is_global{};  // f(1) per ref LAST_FRAME..ALTREF_FRAME (inter frames)
};

/// inverse_recenter(r, v) (AV1 §5.9.29).
[[nodiscard]] int32_t inverse_recenter(int32_t r, int32_t v) noexcept;

/// global_motion_params() (AV1 §5.9.24) with read_global_param(), decode_signed_subexp_with_ref(),
/// decode_unsigned_subexp_with_ref(), decode_subexp() and inverse_recenter() (AV1 §5.9.25–§5.9.29) at the current
/// position (node "global_motion_params"). `PrevGmParams` is the primary reference's SavedGmParams
/// (load_previous()) or identity_gm_params() (setup_past_independence(), and every error-resilient frame).
[[nodiscard]] GlobalMotionParams parse_global_motion_params(BitReader& br, SyntaxBuilder& b, bool FrameIsIntra,
                                                            bool allow_high_precision_mv, const GmParams& PrevGmParams);

// ---- row 27: film_grain_params() ----------------------------------------------------------------------------

/// film_grain_params() (AV1 §5.9.30) at the current position (node "film_grain_params"). Returns the
/// FilmGrainParams the frame uses (what the §7.20 update process saves): reset_grain_params() (all zero) without
/// film_grain_params_present, for a frame that is neither shown nor showable, or with apply_grain = 0. With
/// update_grain = 0, load_grain_params(film_grain_params_ref_idx) copies SavedFilmGrainParams of that slot of
/// `state` (apply_grain included, as libaom does) and grain_seed is restored to the value read; update_grain and
/// film_grain_params_ref_idx keep the values read (they only say where the parameters came from). A
/// film_grain_params_ref_idx that is not one of ref_frame_idx[], num_y_points > 14, num_cb_points or num_cr_points
/// > 10, point values that do not increase, or (4:2:0) exactly one of num_cb_points / num_cr_points zero are
/// Diagnostic Error SYN-AV1-0015 (AV1 §6.8.20 conformance requirements; SPEC-ASSUMPTION(#533)); the values
/// stand as read.
[[nodiscard]] FilmGrainParams parse_film_grain_params(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                                      const DecoderState& state, const RefFrameIdx& ref_frame_idx,
                                                      FrameType frame_type, bool show_frame, bool showable_frame);

/// load_grain_params(idx) (AV1 §7.21 / §6.8.20): the SavedFilmGrainParams of slot `idx`.
[[nodiscard]] FilmGrainParams load_grain_params(const DecoderState& state, uint8_t idx) noexcept;

// ---- row 29: tile_group_obu() -------------------------------------------------------------------------------

/// One tile of a tile group (AV1 §5.11.1 loop body). tile_data() is opaque: byte_offset / tileSize locate it,
/// byte_offset counted from the first byte of the BitReader's data (the caller adds the OBU's file offset for the
/// derived "tile byte range").
struct TileGroupTile {
  uint64_t byte_offset = 0;
  uint64_t tileSize = 0;
  uint32_t TileNum = 0;
  uint32_t tileRow = 0;
  uint32_t tileCol = 0;
  uint32_t MiRowStart = 0;
  uint32_t MiRowEnd = 0;
  uint32_t MiColStart = 0;
  uint32_t MiColEnd = 0;
  uint32_t tile_size_minus_1 = 0;  // le(TileSizeBytes); not read (0) for the last tile of the group
};

struct TileGroup {
  std::vector<TileGroupTile> tiles;  // tg_start .. tg_end
  BitSpan span{0, 0};
  uint32_t tg_start = 0;
  uint32_t tg_end = 0;
  uint32_t headerBytes = 0;  // bytes up to and including byte_alignment()
  bool tile_start_and_end_present_flag = false;
  bool frame_complete = false;  // tg_end == NumTiles - 1: SeenFrameHeader = 0, the reference update runs
};

/// tile_group_obu(sz) (AV1 §5.11.1) at the current position (node "tile_group_obu"), with `sz` the bytes left in
/// the OBU and `ti` the frame's tile_info(). A tile size that overruns the group (or the reader's data) is
/// ParseError SYN-AV1-0020; a tile range outside 0 .. NumTiles - 1 is ParseError SYN-GEN-0003. Either way the
/// frame never reaches tg_end == NumTiles - 1 and the index reports it truncated (SYN-AV1-0101).
[[nodiscard]] TileGroup parse_tile_group_obu(BitReader& br, SyntaxBuilder& b, const TileInfo& ti, uint32_t sz);

}  // namespace ovt::syntax::av1

#endif  // OVT_CXX_SYNTAX_AV1_FRAME_HEADER_HPP
