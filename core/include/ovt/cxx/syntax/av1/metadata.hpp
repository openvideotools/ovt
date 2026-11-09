// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 metadata_obu() (AV1 §5.8): HDR CLL (§5.8.3), HDR MDCV (§5.8.4), scalability (§5.8.5, §5.8.6), ITU-T
// T.35 (§5.8.2) and timecode (§5.8.7) metadata, with the unit conversions into ovt_ColorDescription: chapter 7
// §7.3.6 row 9, WI-1.116. C++ helper header (D22): semi-public, no ABI promise, never included from a C header,
// not installed.

#ifndef OVT_CXX_SYNTAX_AV1_METADATA_HPP
#define OVT_CXX_SYNTAX_AV1_METADATA_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ovt/color.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::av1 {

/// metadata_type values (AV1 §6.7.1). 6..31 are unregistered user private, the rest reserved.
enum class MetadataType : uint8_t {
  HdrCll = 1,
  HdrMdcv = 2,
  Scalability = 3,
  ItutT35 = 4,
  Timecode = 5,
};

inline constexpr uint8_t kScalabilitySs = 14;  // SCALABILITY_SS (AV1 §6.7.5)

/// metadata_hdr_cll() (AV1 §5.8.3). Both values are cd/m², the ovt_ColorDescription units.
struct MetadataHdrCll {
  uint16_t max_cll = 0;
  uint16_t max_fall = 0;
};

/// metadata_hdr_mdcv() (AV1 §5.8.4): 0.16 chromaticities in R, G, B order, 24.8 maximum and 18.14 minimum
/// luminance, with the derived ovt_ColorDescription values (0.00002 chromaticity units, 0.0001 cd/m²).
struct MetadataHdrMdcv {
  std::array<uint16_t, 3> primary_chromaticity_x{};
  std::array<uint16_t, 3> primary_chromaticity_y{};
  uint16_t white_point_chromaticity_x = 0;
  uint16_t white_point_chromaticity_y = 0;
  uint32_t luminance_max = 0;
  uint32_t luminance_min = 0;
  // Derived (rounded to nearest): x * 50000 / 65536; luminance_max * 10000 / 256; luminance_min * 10000 / 16384
  std::array<std::array<uint16_t, 2>, 3> mdcv_primaries{};  // [0] red, [1] green, [2] blue (chapter 10 §10.6.4)
  std::array<uint16_t, 2> mdcv_white_point{};
  uint32_t mdcv_max_luminance = 0;
  uint32_t mdcv_min_luminance = 0;
};

/// scalability_structure() (AV1 §5.8.6).
struct ScalabilityStructure {
  uint8_t spatial_layers_cnt_minus_1 = 0;
  bool spatial_layer_dimensions_present_flag = false;
  bool spatial_layer_description_present_flag = false;
  bool temporal_group_description_present_flag = false;
  uint8_t scalability_structure_reserved_3bits = 0;
  std::vector<uint16_t> spatial_layer_max_width;   // spatial_layers_cnt_minus_1 + 1 entries when present
  std::vector<uint16_t> spatial_layer_max_height;  // "
  std::vector<uint8_t> spatial_layer_ref_id;       // "
  uint8_t temporal_group_size = 0;
  struct TemporalGroup {
    uint8_t temporal_group_temporal_id = 0;
    bool temporal_group_temporal_switching_up_point_flag = false;
    bool temporal_group_spatial_switching_up_point_flag = false;
    uint8_t temporal_group_ref_cnt = 0;
    std::vector<uint8_t> temporal_group_ref_pic_diff;
  };
  std::vector<TemporalGroup> temporal_groups;  // temporal_group_size entries when present
};

/// metadata_scalability() (AV1 §5.8.5).
struct MetadataScalability {
  uint8_t scalability_mode_idc = 0;
  std::optional<ScalabilityStructure> scalability_structure;  // when scalability_mode_idc == SCALABILITY_SS
};

/// metadata_itut_t35() (AV1 §5.8.2) with the identification the §7.3.11 registry made of it. The registered
/// payload itself (HDR10+ fields, …) is parsed again by ovt::syntax::parse_t35() on `t35_message` when needed;
/// the flags below are what ovt_ColorDescription takes.
struct MetadataItutT35 {
  uint8_t itu_t_t35_country_code = 0;
  std::optional<uint8_t> itu_t_t35_country_code_extension_byte;  // when itu_t_t35_country_code == 0xFF
  std::optional<uint16_t> itu_t_t35_terminal_provider_code;
  std::optional<uint32_t> itu_t_t35_terminal_provider_oriented_code;  // 16 or 32 bits per the registry row
  std::string enum_name;  // registry name, e.g. "HDR10+ (SMPTE ST 2094-40)"; empty when unknown
  bool has_hdr10plus = false;
  bool has_dolby_vision = false;
  std::span<const uint8_t> t35_message;  // country code to the end of the OBU payload, trailing bits stripped;
                                         // views the caller's buffer
};

/// metadata_timecode() (AV1 §5.8.7).
struct MetadataTimecode {
  uint8_t counting_type = 0;
  bool full_timestamp_flag = false;
  bool discontinuity_flag = false;
  bool cnt_dropped_flag = false;
  uint16_t n_frames = 0;
  bool seconds_flag = false;  // read only when !full_timestamp_flag; the value flags are then the presence
  bool minutes_flag = false;
  bool hours_flag = false;
  uint8_t seconds_value = 0;
  uint8_t minutes_value = 0;
  uint8_t hours_value = 0;
  uint8_t time_offset_length = 0;
  uint32_t time_offset_value = 0;
  std::string timecode_text;  // derived "HH:MM:SS:FF" with "--" for absent fields
};

/// §7.3.6 row 9: metadata_obu().
struct Metadata {
  uint64_t metadata_type = 0;
  std::optional<MetadataHdrCll> hdr_cll;
  std::optional<MetadataHdrMdcv> hdr_mdcv;
  std::optional<MetadataScalability> scalability;
  std::optional<MetadataItutT35> itut_t35;
  std::optional<MetadataTimecode> timecode;
  std::span<const uint8_t> unknown_payload;  // types 0 and 6+: the bytes up to the trailing bits
  BitSpan span{0, 0};

  [[nodiscard]] bool known_type() const noexcept { return metadata_type >= 1 && metadata_type <= 5; }
};

/// Parses metadata_obu() at the current position, followed by the OBU's trailing_bits() (node "metadata_obu").
/// Types 0 and 6+ give the node "metadata_unknown" with the payload bytes and Diagnostic Info SYN-AV1-0005
/// (SPEC-ASSUMPTION(#321)); a reserved scalability_mode_idc is Warning SYN-AV1-0004. A T.35 payload the
/// §7.3.11 registry parses (HDR10+) throws ParseError when its structure is malformed (§7.3.1 rule 1).
[[nodiscard]] Metadata parse_metadata(BitReader& br, SyntaxBuilder& b);

/// Copies the static HDR metadata of `m` into `color` (mdcv_*, max_cll/max_fall, has_hdr10plus,
/// has_dolby_vision); fields `m` does not carry are left unchanged.
void apply_metadata(const Metadata& m, ovt_ColorDescription& color) noexcept;

}  // namespace ovt::syntax::av1

#endif  // OVT_CXX_SYNTAX_AV1_METADATA_HPP
