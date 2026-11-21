// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 SEI messages (Annex D): sei_rbsp() framing and the payload parsers of chapter 7 §7.3.4 rows 7–18
// (WI-1.110a: framing, buffering_period, pic_timing; WI-1.110b: the picture-level and user-data payloads of rows 10–17
// plus film grain / tone mapping). C++ helper header (D22): semi-public, no ABI promise, never included from a C
// header.
//
// The reader stands after the NAL header (parse_nal_header()). Every message becomes a node `sei_message[i]`
// whose child is the payload node named after the payload (`buffering_period`, `pic_timing`, …, or
// `reserved_sei_message` for an unlisted type); payload types without a parser keep their bytes under the
// Annex D name (Bytes). A payload that fails to parse is reported as an Error Diagnostic on its message and the
// next message is still parsed; a payloadSize beyond the RBSP stops the loop (ParseError BR-0001, one Diagnostic).

#ifndef OVT_CXX_SYNTAX_H264_SEI_HPP
#define OVT_CXX_SYNTAX_H264_SEI_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264 {

/// buffering_period() (D.1.2, type 0).
struct BufferingPeriod {
  uint32_t seq_parameter_set_id = 0;
  std::vector<uint32_t> nal_initial_cpb_removal_delay;  // per SchedSelIdx; empty without NAL HRD
  std::vector<uint32_t> nal_initial_cpb_removal_delay_offset;
  std::vector<uint32_t> vcl_initial_cpb_removal_delay;  // empty without VCL HRD
  std::vector<uint32_t> vcl_initial_cpb_removal_delay_offset;
};

/// One clock timestamp of pic_timing() (D.1.3).
struct ClockTimestamp {
  bool clock_timestamp_flag = false;
  uint32_t ct_type = 0;
  bool nuit_field_based_flag = false;
  uint32_t counting_type = 0;
  bool full_timestamp_flag = false;
  bool discontinuity_flag = false;
  bool cnt_dropped_flag = false;
  uint32_t n_frames = 0;
  bool seconds_flag = false;
  bool minutes_flag = false;
  bool hours_flag = false;
  uint32_t seconds_value = 0;
  uint32_t minutes_value = 0;
  uint32_t hours_value = 0;
  int32_t time_offset = 0;
  std::string text;  // "hh:mm:ss:ff" (D.2.3 fields; a field that was not sent is 0)
};

/// pic_timing() (D.1.3, type 1).
struct PicTiming {
  bool sps_known = false;  // false: no SPS was available, only the framing was recorded (Diagnostic SYN-H264-0031)
  bool cpb_dpb_delays_present = false;
  uint32_t cpb_removal_delay = 0;
  uint32_t dpb_output_delay = 0;
  bool pic_struct_present = false;
  uint32_t pic_struct = 0;
  std::string_view pic_struct_name;  // Table D-1
  uint32_t NumClockTS = 0;
  std::vector<ClockTimestamp> clock_timestamps;
};

struct RecoveryPoint {  // D.1.8, type 6
  uint32_t recovery_frame_cnt = 0;
  bool exact_match_flag = false;
  bool broken_link_flag = false;
  uint32_t changing_slice_group_idc = 0;
};

struct UserDataRegisteredItuTT35 {  // D.1.6, type 4 (identification fields only; the payload stays in the tree)
  uint8_t itu_t_t35_country_code = 0;
  std::optional<uint8_t> itu_t_t35_country_code_extension_byte;
  std::optional<uint16_t> itu_t_t35_terminal_provider_code;
  std::optional<uint32_t> itu_t_t35_terminal_provider_oriented_code;
  std::string enum_name;  // registry name (§7.3.11) or "T.35 provider …"
  bool has_hdr10plus = false;
  bool has_dolby_vision = false;
  uint32_t payload_byte_count = 0;
};

struct SeiUserDataUnregistered {  // D.1.7, type 5
  std::string uuid;               // canonical upper-case 8-4-4-4-12
  std::string enum_name;          // "x264 options", … or "user data (UUID …)"
  std::optional<std::string> text;
  uint32_t payload_byte_count = 0;
};

struct FramePackingArrangement {  // D.1.26, type 45
  uint32_t frame_packing_arrangement_id = 0;
  bool cancel_flag = false;
  uint32_t type = 0;
  std::string_view type_name;  // Table D-8
  bool quincunx_sampling_flag = false;
  uint32_t content_interpretation_type = 0;
  bool spatial_flipping_flag = false;
  bool frame0_flipped_flag = false;
  bool field_views_flag = false;
  bool current_frame_is_frame0_flag = false;
  bool frame0_self_contained_flag = false;
  bool frame1_self_contained_flag = false;
  std::array<uint32_t, 4> grid_position{};  // frame0 x, frame0 y, frame1 x, frame1 y
  uint32_t reserved_byte = 0;
  uint32_t repetition_period = 0;
  bool extension_flag = false;
};

struct DisplayOrientation {  // D.1.27, type 47
  bool cancel_flag = false;
  bool hor_flip = false;
  bool ver_flip = false;
  uint32_t anticlockwise_rotation = 0;
  double degrees = 0.0;  // anticlockwise_rotation * 360 / 65536
  uint32_t repetition_period = 0;
  bool extension_flag = false;
};

struct MasteringDisplayColourVolume {             // D.1.29, type 137
  std::array<uint32_t, 3> display_primaries_x{};  // as coded: c = 0 green, 1 blue, 2 red
  std::array<uint32_t, 3> display_primaries_y{};
  uint32_t white_point_x = 0;
  uint32_t white_point_y = 0;
  uint32_t max_display_mastering_luminance = 0;  // 0.0001 cd/m²
  uint32_t min_display_mastering_luminance = 0;
  // ovt_ColorDescription order (R, G, B), units of 0.00002
  std::array<uint32_t, 3> mdcv_primaries_x{};
  std::array<uint32_t, 3> mdcv_primaries_y{};
  std::string text;  // "R (0.708, 0.292) G (…) B (…) WP (…) max 1000 cd/m² min 0.0001 cd/m²"
};

struct ContentLightLevelInfo {  // D.1.30, type 144
  uint32_t max_content_light_level = 0;
  uint32_t max_pic_average_light_level = 0;
};

struct AlternativeTransferCharacteristics {  // D.1.31, type 147
  uint32_t preferred_transfer_characteristics = 0;
};

struct FilmGrainComponent {
  uint32_t num_intensity_intervals_minus1 = 0;
  uint32_t num_model_values_minus1 = 0;
  std::vector<uint32_t> intensity_interval_lower_bound;
  std::vector<uint32_t> intensity_interval_upper_bound;
  std::vector<int32_t> comp_model_value;  // (num_intensity_intervals_minus1+1) x (num_model_values_minus1+1)
};

struct FilmGrainCharacteristics {  // D.1.20, type 19
  bool cancel_flag = false;
  uint32_t film_grain_model_id = 0;
  bool separate_colour_description_present_flag = false;
  uint32_t bit_depth_luma_minus8 = 0;
  uint32_t bit_depth_chroma_minus8 = 0;
  bool full_range_flag = false;
  uint32_t colour_primaries = 0;
  uint32_t transfer_characteristics = 0;
  uint32_t matrix_coefficients = 0;
  uint32_t blending_mode_id = 0;
  uint32_t log2_scale_factor = 0;
  std::array<bool, 3> comp_model_present_flag{};
  std::array<FilmGrainComponent, 3> comp;
  uint32_t repetition_period = 0;
};

struct ToneMappingInfo {  // D.1.21, type 23
  uint32_t tone_map_id = 0;
  bool cancel_flag = false;
  uint32_t repetition_period = 0;
  uint32_t coded_data_bit_depth = 0;
  uint32_t target_bit_depth = 0;
  uint32_t tone_map_model_id = 0;
  std::vector<uint32_t> values;  // model 0: min, max; 1: midpoint, width; 2: intervals; 3: coded/target pivot pairs
};

/// The parsed form of one message; std::monostate when the payload is kept as bytes.
using SeiPayload =
    std::variant<std::monostate, BufferingPeriod, PicTiming, RecoveryPoint, UserDataRegisteredItuTT35,
                 SeiUserDataUnregistered, FramePackingArrangement, DisplayOrientation, MasteringDisplayColourVolume,
                 ContentLightLevelInfo, AlternativeTransferCharacteristics, FilmGrainCharacteristics, ToneMappingInfo>;

struct SeiMessage {
  uint32_t payload_type = 0;
  uint32_t payload_size = 0;
  std::string_view name;  // Annex D name (Table of 0..56) or "reserved_sei_message"
  BitSpan payload_span{0, 0};
  bool parsed = false;  // false: kept as Bytes (no parser, SPS missing) or the payload failed to parse
  SeiPayload payload;
};

/// The parameter sets the SEI parsers look at (D.2.2): buffering_period() names its SPS by id, pic_timing()
/// uses the SPS of the previous picture unless a buffering_period() of the same access unit names one.
struct SeiContext {
  std::span<const Sps* const> sps_by_id;  // indexed by seq_parameter_set_id (0..31); null entries are unknown
  const Sps* active_sps = nullptr;        // SPS of the previous picture
  /// Set by parse_sei_rbsp() from a buffering_period() of this access unit; the caller clears it at the next one.
  std::optional<uint32_t> buffering_period_sps_id;
};

struct SeiRbsp {
  std::vector<SeiMessage> messages;
  bool complete = true;  // false when the framing stopped early (payloadSize beyond the RBSP)
  BitSpan span{0, 0};
};

/// Table of Annex D payload names for payloadType 0..56 (+ 137, 144, 147); "reserved_sei_message" otherwise.
[[nodiscard]] std::string_view sei_payload_name(uint32_t payload_type) noexcept;

/// sei_rbsp() (§7.3.2.3). Node `sei_rbsp`. Only a failure to read the framing itself stops the loop; it is not
/// thrown but recorded (SeiRbsp::complete, Diagnostic on the message).
SeiRbsp parse_sei_rbsp(BitReader& br, SyntaxBuilder& b, SeiContext& ctx);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_SEI_HPP
