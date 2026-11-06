// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ITU-T T.35 and user_data_unregistered registries (chapter 7 §7.3.11, WI-1.120).
//
// Codec-neutral parsers shared by the H.264/H.265 SEI parsers and the AV1 metadata OBU parser. They take
// the payload bytes of the T.35 / user_data_unregistered message, identify them with a constexpr registry
// and fill plain structs; the syntax layer turns the structs into SyntaxNodes (it owns the SyntaxBuilder,
// which is not a dependency of this unit). Malformed input throws ovt::ParseError, as every parser of
// untrusted bytes does (chapter 0 §0.7.1).

#ifndef OVT_SYNTAX_COMMON_USER_DATA_REGISTRY_H
#define OVT_SYNTAX_COMMON_USER_DATA_REGISTRY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ovt/cxx/bitreader.hpp"

namespace ovt::syntax {

// ---- ITU-T T.35 --------------------------------------------------------------------------------------

enum class T35Kind : uint8_t {
  Hdr10Plus,        // ST 2094-40, fully parsed
  AtscGa94,         // ATSC A/53 GA94, cc_data() counted
  DolbyVisionAv1,   // Dolby Vision RPU in an AV1 T.35 OBU, opaque
  HdrVivid,         // CUVA, opaque
  UnknownProvider,  // country 0xB5, provider not in the registry
  UnknownCountry,   // country without a provider field
};

enum class OrientedCode : uint8_t { None, U16, U32 };

// One row of the 7.3.11 T.35 table. Adding a row needs no code change elsewhere.
struct T35Entry {
  uint8_t country_code;
  uint16_t provider_code;
  OrientedCode oriented_width;  // width of itu_t_t35_terminal_provider_oriented_code in the stream
  uint32_t oriented_code;       // expected value when oriented_width != None
  T35Kind kind;
  std::string_view enum_name;
};

inline constexpr uint8_t kT35CountryUs = 0xB5;
inline constexpr uint8_t kT35CountryChina = 0x26;

// SPEC-ASSUMPTION(#89): the Dolby row matches the 32-bit provider-oriented code 0x00000800 of the
// AV1 T.35 OBU (WI-1.120 pitfalls); the 7.3.11 table only lists the leading 16-bit part 0x0000.
inline constexpr std::array<T35Entry, 4> kT35Registry{{
    {kT35CountryUs, 0x003C, OrientedCode::U16, 0x0001, T35Kind::Hdr10Plus, "HDR10+ (SMPTE ST 2094-40)"},
    {kT35CountryUs, 0x0031, OrientedCode::None, 0, T35Kind::AtscGa94, "ATSC A/53 (GA94) CEA-708 captions"},
    {kT35CountryUs, 0x003B, OrientedCode::U32, 0x00000800, T35Kind::DolbyVisionAv1, "Dolby Vision RPU in T.35 (AV1)"},
    {kT35CountryChina, 0x0004, OrientedCode::None, 0, T35Kind::HdrVivid, "HDR Vivid (T/UWA 005)"},
}};

// ST 2094-40 (HDR10+) application_version values OVT parses (7.3.11).
inline constexpr uint8_t kHdr10PlusApplicationIdentifier = 4;

struct Hdr10PlusWindow {  // extra processing windows 1..num_windows-1 (ellipse parameters)
  uint16_t window_upper_left_corner_x, window_upper_left_corner_y;
  uint16_t window_lower_right_corner_x, window_lower_right_corner_y;
  uint16_t center_of_ellipse_x, center_of_ellipse_y;
  uint8_t rotation_angle;
  uint16_t semimajor_axis_internal_ellipse, semimajor_axis_external_ellipse, semiminor_axis_external_ellipse;
  uint8_t overlap_process_option;
};

struct Hdr10PlusGrid {  // targeted_system_display_actual_peak_luminance / mastering_display_actual_peak_luminance
  uint8_t num_rows = 0;
  uint8_t num_cols = 0;
  std::vector<uint8_t> actual_peak_luminance;  // num_rows * num_cols values, u(4), row major
};

struct Hdr10PlusPercentile {
  uint8_t percentage;   // distribution_maxrgb_percentages[i], u(7)
  uint32_t percentile;  // distribution_maxrgb_percentiles[i], u(17)
};

struct Hdr10PlusToneMapping {
  uint16_t knee_point_x = 0, knee_point_y = 0;  // u(12)
  std::vector<uint16_t> bezier_curve_anchors;   // u(10), num_bezier_curve_anchors entries
};

struct Hdr10PlusProcessingWindow {   // per window i in 0..num_windows-1
  std::array<uint32_t, 3> maxscl{};  // u(17)
  uint32_t average_maxrgb = 0;       // u(17)
  std::vector<Hdr10PlusPercentile> distribution_maxrgb;
  uint16_t fraction_bright_pixels = 0;               // u(10)
  std::optional<Hdr10PlusToneMapping> tone_mapping;  // present when tone_mapping_flag
  std::optional<uint8_t> color_saturation_weight;    // present when color_saturation_mapping_flag, u(6)
};

struct Hdr10Plus {
  uint8_t application_identifier = 0;
  uint8_t application_version = 0;
  uint8_t num_windows = 0;                                 // 1..3
  std::vector<Hdr10PlusWindow> windows;                    // num_windows - 1 entries
  uint32_t targeted_system_display_maximum_luminance = 0;  // u(27)
  std::optional<Hdr10PlusGrid> targeted_system_display_actual_peak_luminance;
  std::vector<Hdr10PlusProcessingWindow> processing;  // num_windows entries
  std::optional<Hdr10PlusGrid> mastering_display_actual_peak_luminance;
};

// ATSC A/53 GA94 payload: counts only; caption bytes are not decoded in phase 1.
struct AtscCaptions {
  uint32_t user_identifier = 0;     // 0x47413934 'GA94'
  uint8_t user_data_type_code = 0;  // 0x03 cc_data, 0x06 bar_data, other: opaque
  bool is_cc_data = false;          // the fields below are valid only then
  bool process_em_data_flag = false, process_cc_data_flag = false, additional_data_flag = false;
  uint8_t cc_count = 0;  // u(5)
  uint8_t em_data = 0;
  uint32_t valid_608_packets = 0;  // cc_valid && cc_type in {0, 1}
  uint32_t valid_708_packets = 0;  // cc_valid && cc_type in {2, 3}
  uint32_t invalid_packets = 0;    // !cc_valid
};

inline constexpr uint32_t kGa94UserIdentifier = 0x47413934;

struct T35Result {
  uint8_t country_code = 0;
  std::optional<uint8_t> country_code_extension_byte;  // when country_code == 0xFF
  std::optional<uint16_t> provider_code;
  std::optional<uint32_t> provider_oriented_code;  // value as read (16 or 32 bit per the matched entry)
  std::string enum_name;
  std::optional<T35Kind> kind;
  bool has_hdr10plus = false;     // ovt_ColorDescription.has_hdr10plus
  bool has_dolby_vision = false;  // ovt_ColorDescription.has_dolby_vision
  std::optional<Hdr10Plus> hdr10plus;
  std::optional<AtscCaptions> atsc;
  std::span<const uint8_t> payload;  // bytes after the identification fields; views the caller's buffer
};

enum class T35Carrier : uint8_t {
  Sei,          // H.264 / H.265 user_data_registered_itu_t_t35 SEI payload
  Av1Metadata,  // AV1 metadata_itut_t35 payload (trailing bits are inside the payload bytes)
};

// Parses a T.35 message whose first byte is itu_t_t35_country_code. Throws ovt::ParseError on truncated
// or malformed input.
[[nodiscard]] T35Result parse_t35(std::span<const uint8_t> payload, T35Carrier carrier = T35Carrier::Sei);

// AV1 T.35 payloads include the OBU's trailing bits: drops a final 0x80 byte with the zero bytes after it
// (7.3.11 pitfall). A payload whose last non-zero byte is not 0x80 is returned unchanged.
[[nodiscard]] std::span<const uint8_t> strip_av1_trailing_bits(std::span<const uint8_t> payload) noexcept;

// Individual parsers, exposed for tests; the reader is positioned after the identification fields.
[[nodiscard]] Hdr10Plus parse_hdr10plus(BitReader& br);   // t35_hdr10plus.cpp; starts at application_identifier
[[nodiscard]] AtscCaptions parse_atsc_cc(BitReader& br);  // t35_atsc_cc.cpp; starts at user_identifier

// ---- user_data_unregistered --------------------------------------------------------------------------

using Uuid = std::array<uint8_t, 16>;

struct UuidEntry {
  Uuid uuid;  // bytes as they appear in the stream
  std::string_view enum_name;
  std::string_view option_prefix;  // child node name prefix, e.g. "x264_option"; empty: no option children
};

inline constexpr std::array<UuidEntry, 2> kUuidRegistry{{
    {{0xDC, 0x45, 0xE9, 0xBD, 0xE6, 0xD9, 0x48, 0xB7, 0x96, 0x2C, 0xD8, 0x20, 0xD9, 0x23, 0xEE, 0xEF},
     "x264 options",
     "x264_option"},
    {{0x2C, 0xA2, 0xDE, 0x09, 0xB5, 0x17, 0x47, 0xDB, 0xBB, 0x55, 0xA4, 0xFE, 0x7F, 0xC2, 0xFC, 0x4E},
     "x265 options",
     "x265_option"},
}};

struct EncoderOption {
  std::string name;
  std::string value;  // empty for flag options without '=' (x265 "wpp")
  bool has_value = false;
};

struct UserDataUnregistered {
  Uuid uuid{};
  std::string uuid_text;  // canonical 8-4-4-4-12 text, upper-case hex
  std::string enum_name;  // registry name or "user data (UUID <text>)"
  bool known = false;
  std::string_view option_prefix;
  std::span<const uint8_t> payload;    // user_data_payload_byte[], views the caller's buffer
  std::optional<std::string> text;     // set when every payload byte is printable ASCII (trailing NULs ignored)
  std::vector<EncoderOption> options;  // x264 / x265 key=value list; empty for other UUIDs
};

// Parses a user_data_unregistered payload (16-byte UUID + data). Throws ovt::ParseError (BR-0001) if
// shorter than 16 bytes.
[[nodiscard]] UserDataUnregistered parse_user_data_unregistered(std::span<const uint8_t> payload);

// Splits the text after "options:" into name[=value] entries separated by spaces. Text without an
// "options:" marker gives no entries.
[[nodiscard]] std::vector<EncoderOption> split_encoder_options(std::string_view text);

[[nodiscard]] std::string uuid_to_string(const Uuid& uuid);

// ---- Dolby Vision markers outside SEI/T.35 ------------------------------------------------------------

struct DolbyVisionMarker {
  std::string_view enum_name;
  bool has_dolby_vision;
};

// HEVC unspecified NAL types: 62 with nuh_layer_id 0 → "Dolby Vision RPU", 63 → "Dolby Vision EL".
[[nodiscard]] std::optional<DolbyVisionMarker> classify_hevc_unspecified_nal(uint8_t nal_unit_type,
                                                                             uint8_t nuh_layer_id) noexcept;

// H.264 NAL type 28 is a DV RPU only when the container signals Dolby Vision (dvcC).
[[nodiscard]] std::optional<DolbyVisionMarker> classify_h264_nal28(bool container_signals_dolby_vision) noexcept;

}  // namespace ovt::syntax

#endif  // OVT_SYNTAX_COMMON_USER_DATA_REGISTRY_H
