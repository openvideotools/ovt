// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 sequence_header_obu() (AV1 §5.5.1) with timing_info() (§5.5.3), decoder_model_info() (§5.5.4),
// operating_parameters_info() (§5.5.5) and color_config() (§5.5.2): chapter 7 §7.3.6 rows 2–6, WI-1.116.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header, not installed.
//
// The typed structs hold every syntax element under its spec name and every derived variable (§7.3.7); the
// SyntaxNode tree is for display. Fields the syntax table infers instead of reading carry the inferred value.

#ifndef OVT_CXX_SYNTAX_AV1_SEQUENCE_HEADER_HPP
#define OVT_CXX_SYNTAX_AV1_SEQUENCE_HEADER_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/color.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::av1 {

inline constexpr uint8_t kSelectScreenContentTools = 2;  // SELECT_SCREEN_CONTENT_TOOLS (AV1 §3)
inline constexpr uint8_t kSelectIntegerMv = 2;           // SELECT_INTEGER_MV
inline constexpr uint8_t kMaxOperatingPoints = 32;       // operating_points_cnt_minus_1 is f(5)

// H.273 code points the sequence header infers (AV1 §6.4.2).
inline constexpr uint8_t kCpBt709 = 1;
inline constexpr uint8_t kCpUnspecified = 2;
inline constexpr uint8_t kTcUnspecified = 2;
inline constexpr uint8_t kTcSrgb = 13;
inline constexpr uint8_t kMcIdentity = 0;
inline constexpr uint8_t kMcUnspecified = 2;

/// chroma_sample_position values (AV1 §6.4.2).
enum class ChromaSamplePosition : uint8_t { Unknown = 0, Vertical = 1, Colocated = 2, Reserved = 3 };

/// §7.3.6 row 3: timing_info().
struct TimingInfo {
  uint32_t num_units_in_display_tick = 0;
  uint32_t time_scale = 0;
  bool equal_picture_interval = false;
  uint32_t num_ticks_per_picture_minus_1 = 0;
  // Derived: frames per second when equal_picture_interval (time_scale / (num_units_in_display_tick *
  // (num_ticks_per_picture_minus_1 + 1))); 0 otherwise.
  double frame_rate = 0.0;
  BitSpan span{0, 0};
};

/// §7.3.6 row 4: decoder_model_info().
struct DecoderModelInfo {
  uint8_t buffer_delay_length_minus_1 = 0;
  uint32_t num_units_in_decoding_tick = 0;
  uint8_t buffer_removal_time_length_minus_1 = 0;
  uint8_t frame_presentation_time_length_minus_1 = 0;
  BitSpan span{0, 0};
};

/// §7.3.6 row 5: operating_parameters_info(op).
struct OperatingParametersInfo {
  uint32_t decoder_buffer_delay = 0;
  uint32_t encoder_buffer_delay = 0;
  bool low_delay_mode_flag = false;
  BitSpan span{0, 0};
};

/// One operating point i of the sequence header (AV1 §5.5.1 loop body).
struct OperatingPoint {
  uint16_t operating_point_idc = 0;
  uint8_t seq_level_idx = 0;
  bool seq_tier = false;
  bool decoder_model_present_for_this_op = false;
  OperatingParametersInfo operating_parameters_info;  // valid when decoder_model_present_for_this_op
  bool initial_display_delay_present_for_this_op = false;
  uint8_t initial_display_delay_minus_1 = 0;
  // Derived: "2.0" … "7.3", "Maximum parameters" for 31, "Reserved" otherwise; "Main" / "High".
  std::string level_name;
  std::string_view tier_name;
};

/// §7.3.6 row 6: color_config().
struct ColorConfig {
  bool high_bitdepth = false;
  bool twelve_bit = false;
  bool mono_chrome = false;
  bool color_description_present_flag = false;
  uint8_t color_primaries = kCpUnspecified;
  uint8_t transfer_characteristics = kTcUnspecified;
  uint8_t matrix_coefficients = kMcUnspecified;
  bool color_range = false;
  bool subsampling_x = true;
  bool subsampling_y = true;
  ChromaSamplePosition chroma_sample_position = ChromaSamplePosition::Unknown;
  bool separate_uv_delta_q = false;
  // Derived
  uint8_t BitDepth = 8;
  uint8_t NumPlanes = 3;
  uint8_t chroma_format = 1;  // ovt_ColorDescription.chroma_format: 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4
  uint8_t chroma_siting = 0;  // ovt_ColorDescription.chroma_siting: COLOCATED → 3, VERTICAL → 1, else 0
  BitSpan span{0, 0};
};

/// §7.3.6 row 2: sequence_header_obu().
struct SequenceHeader {
  // Members are grouped by size, largest first, so the struct carries no padding (clang-analyzer
  // optin.performance.Padding); the comments give the syntax order of AV1 §5.5.1.
  std::vector<OperatingPoint> operating_points;  // operating_points_cnt_minus_1 + 1 entries
  TimingInfo timing_info;                        // valid when timing_info_present_flag
  DecoderModelInfo decoder_model_info;           // valid when decoder_model_info_present_flag
  ColorConfig color_config;
  std::string_view profile_name;  // derived: "Main", "High", "Professional"
  BitSpan span{0, 0};
  uint32_t max_frame_width_minus_1 = 0;
  uint32_t max_frame_height_minus_1 = 0;
  uint32_t max_frame_width = 0;   // derived
  uint32_t max_frame_height = 0;  // derived
  // seq_profile … initial_display_delay_present_flag, then operating_points[]
  uint8_t seq_profile = 0;
  bool still_picture = false;
  bool reduced_still_picture_header = false;
  bool timing_info_present_flag = false;
  bool decoder_model_info_present_flag = false;
  bool initial_display_delay_present_flag = false;
  uint8_t operating_points_cnt_minus_1 = 0;
  // frame size fields (max_frame_*_minus_1 above), frame ids
  uint8_t frame_width_bits_minus_1 = 0;
  uint8_t frame_height_bits_minus_1 = 0;
  bool frame_id_numbers_present_flag = false;
  uint8_t delta_frame_id_length_minus_2 = 0;
  uint8_t additional_frame_id_length_minus_1 = 0;
  // coding tools
  bool use_128x128_superblock = false;
  bool enable_filter_intra = false;
  bool enable_intra_edge_filter = false;
  bool enable_interintra_compound = false;
  bool enable_masked_compound = false;
  bool enable_warped_motion = false;
  bool enable_dual_filter = false;
  bool enable_order_hint = false;
  bool enable_jnt_comp = false;
  bool enable_ref_frame_mvs = false;
  bool seq_choose_screen_content_tools = false;
  uint8_t seq_force_screen_content_tools = kSelectScreenContentTools;
  bool seq_choose_integer_mv = false;
  uint8_t seq_force_integer_mv = kSelectIntegerMv;
  uint8_t order_hint_bits_minus_1 = 0;
  bool enable_superres = false;
  bool enable_cdef = false;
  bool enable_restoration = false;
  // color_config above, then film_grain_params_present
  bool film_grain_params_present = false;
  uint8_t OrderHintBits = 0;  // derived: order_hint_bits_minus_1 + 1, or 0 without enable_order_hint

  /// operating_point_idc of operating point `index`, or 0 (every layer) when out of range.
  [[nodiscard]] uint16_t operating_point_idc(size_t index) const noexcept;
};

/// Parses sequence_header_obu() at the current position and its trailing_bits() to the end of the OBU (node
/// "sequence_header_obu"). seq_profile > 2 throws ParseError SYN-GEN-0003 before anything else is read
/// (§7.3.6 row 2). Other reserved values are Warnings (SPEC-ASSUMPTION(#321): SYN-AV1-0004).
[[nodiscard]] SequenceHeader parse_sequence_header(BitReader& br, SyntaxBuilder& b);

// The sub-structures, exposed for the frame header parser (WI-1.117) and tests. Each one reads the syntax
// table of its row at the current position inside the current struct.
[[nodiscard]] TimingInfo parse_timing_info(BitReader& br, SyntaxBuilder& b);
[[nodiscard]] DecoderModelInfo parse_decoder_model_info(BitReader& br, SyntaxBuilder& b);
[[nodiscard]] OperatingParametersInfo parse_operating_parameters_info(BitReader& br, SyntaxBuilder& b, int op,
                                                                      const DecoderModelInfo& model);
[[nodiscard]] ColorConfig parse_color_config(BitReader& br, SyntaxBuilder& b, uint8_t seq_profile);

/// The ovt_ColorDescription candidate of §7.3.6 row 2: H.273 code points, range, chroma siting and format,
/// bit depth. The static HDR metadata fields stay 0 until a metadata OBU fills them (metadata.hpp).
[[nodiscard]] ovt_ColorDescription color_description(const SequenceHeader& seq) noexcept;

/// Level and tier names of AV1 Annex A (A.3): "2.0" … "7.3", "Maximum parameters" (31), "Reserved".
[[nodiscard]] std::string level_name(uint8_t seq_level_idx);
[[nodiscard]] std::string_view tier_name(bool seq_tier) noexcept;

}  // namespace ovt::syntax::av1

#endif  // OVT_CXX_SYNTAX_AV1_SEQUENCE_HEADER_HPP
