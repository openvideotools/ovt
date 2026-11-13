// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 metadata_obu() (chapter 7 §7.3.6 row 9; WI-1.116). metadata_itut_t35() hands the message to the
// §7.3.11 registry (WI-1.120) for identification and emits the identification fields it consumed as nodes.

#include "ovt/cxx/syntax/av1/metadata.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "syntax/common/user_data_registry.h"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecMetadata = "AV1 5.8.1";

// 0.16 fixed-point chromaticity → ovt_ColorDescription 0.00002 units (×50000/65536), rounded to nearest.
uint16_t chromaticity_units(uint16_t v) noexcept {
  return static_cast<uint16_t>((static_cast<uint32_t>(v) * 50000U + 32768U) >> 16U);
}

// Fixed-point luminance → 0.0001 cd/m² units: value * 10000 / 2^frac_bits, rounded to nearest.
uint32_t luminance_units(uint32_t v, unsigned frac_bits) noexcept {
  const uint64_t scaled = static_cast<uint64_t>(v) * 10000U + (uint64_t{1} << (frac_bits - 1));
  return static_cast<uint32_t>(scaled >> frac_bits);
}

std::string_view metadata_type_name(uint64_t t) noexcept {
  switch (t) {
    case 1:
      return "METADATA_TYPE_HDR_CLL";
    case 2:
      return "METADATA_TYPE_HDR_MDCV";
    case 3:
      return "METADATA_TYPE_SCALABILITY";
    case 4:
      return "METADATA_TYPE_ITUT_T35";
    case 5:
      return "METADATA_TYPE_TIMECODE";
    default:
      return t >= 6 && t <= 31 ? "Unregistered user private" : "Reserved";
  }
}

MetadataHdrCll parse_hdr_cll(SyntaxBuilder& b) {
  MetadataHdrCll m;
  auto s = b.scoped("metadata_hdr_cll", "AV1 5.8.3");
  m.max_cll = static_cast<uint16_t>(b.read_u("max_cll", 16));
  m.max_fall = static_cast<uint16_t>(b.read_u("max_fall", 16));
  return m;
}

MetadataHdrMdcv parse_hdr_mdcv(SyntaxBuilder& b) {
  MetadataHdrMdcv m;
  auto s = b.scoped("metadata_hdr_mdcv", "AV1 5.8.4");
  for (int i = 0; i < 3; ++i) {  // R, G, B (AV1 §6.7.4)
    b.begin_array_element("primary_chromaticity", i);
    m.primary_chromaticity_x[static_cast<size_t>(i)] = static_cast<uint16_t>(b.read_u("primary_chromaticity_x", 16));
    m.primary_chromaticity_y[static_cast<size_t>(i)] = static_cast<uint16_t>(b.read_u("primary_chromaticity_y", 16));
    b.end_array_element();
  }
  m.white_point_chromaticity_x = static_cast<uint16_t>(b.read_u("white_point_chromaticity_x", 16));
  m.white_point_chromaticity_y = static_cast<uint16_t>(b.read_u("white_point_chromaticity_y", 16));
  m.luminance_max = static_cast<uint32_t>(b.read_u("luminance_max", 32));
  m.luminance_min = static_cast<uint32_t>(b.read_u("luminance_min", 32));
  for (size_t i = 0; i < 3; ++i) {
    m.mdcv_primaries[i] = {chromaticity_units(m.primary_chromaticity_x[i]),
                           chromaticity_units(m.primary_chromaticity_y[i])};
  }
  m.mdcv_white_point = {chromaticity_units(m.white_point_chromaticity_x),
                        chromaticity_units(m.white_point_chromaticity_y)};
  m.mdcv_max_luminance = luminance_units(m.luminance_max, 8);   // 24.8
  m.mdcv_min_luminance = luminance_units(m.luminance_min, 14);  // 18.14
  b.derived("mdcv_primaries", fmt::format("mdcv_primaries (R,G,B; 0.00002) = {{{},{}}},{{{},{}}},{{{},{}}}",
                                          m.mdcv_primaries[0][0], m.mdcv_primaries[0][1], m.mdcv_primaries[1][0],
                                          m.mdcv_primaries[1][1], m.mdcv_primaries[2][0], m.mdcv_primaries[2][1]));
  b.derived("mdcv_white_point",
            fmt::format("mdcv_white_point (0.00002) = {{{},{}}}", m.mdcv_white_point[0], m.mdcv_white_point[1]));
  b.derived("mdcv_max_luminance", fmt::format("mdcv_max_luminance = {} (0.0001 cd/m2)", m.mdcv_max_luminance));
  b.derived("mdcv_min_luminance", fmt::format("mdcv_min_luminance = {} (0.0001 cd/m2)", m.mdcv_min_luminance));
  return m;
}

ScalabilityStructure parse_scalability_structure(SyntaxBuilder& b) {
  ScalabilityStructure ss;
  auto s = b.scoped("scalability_structure", "AV1 5.8.6");
  ss.spatial_layers_cnt_minus_1 = static_cast<uint8_t>(b.read_u("spatial_layers_cnt_minus_1", 2));
  ss.spatial_layer_dimensions_present_flag = b.read_flag("spatial_layer_dimensions_present_flag");
  ss.spatial_layer_description_present_flag = b.read_flag("spatial_layer_description_present_flag");
  ss.temporal_group_description_present_flag = b.read_flag("temporal_group_description_present_flag");
  ss.scalability_structure_reserved_3bits =
      static_cast<uint8_t>(b.read_f("scalability_structure_reserved_3bits", 3, 0));
  const int layers = ss.spatial_layers_cnt_minus_1 + 1;
  if (ss.spatial_layer_dimensions_present_flag) {
    for (int i = 0; i < layers; ++i) {
      b.begin_array_element("spatial_layer", i);
      ss.spatial_layer_max_width.push_back(static_cast<uint16_t>(b.read_u("spatial_layer_max_width", 16)));
      ss.spatial_layer_max_height.push_back(static_cast<uint16_t>(b.read_u("spatial_layer_max_height", 16)));
      b.end_array_element();
    }
  }
  if (ss.spatial_layer_description_present_flag) {
    for (int i = 0; i < layers; ++i) {
      b.begin_array_element("spatial_layer_ref_id", i);
      ss.spatial_layer_ref_id.push_back(static_cast<uint8_t>(b.read_u("spatial_layer_ref_id", 8)));
      b.end_array_element();
    }
  }
  if (ss.temporal_group_description_present_flag) {
    ss.temporal_group_size = static_cast<uint8_t>(b.read_u("temporal_group_size", 8));
    for (int i = 0; i < ss.temporal_group_size; ++i) {
      ScalabilityStructure::TemporalGroup g;
      b.begin_array_element("temporal_group", i);
      g.temporal_group_temporal_id = static_cast<uint8_t>(b.read_u("temporal_group_temporal_id", 3));
      g.temporal_group_temporal_switching_up_point_flag =
          b.read_flag("temporal_group_temporal_switching_up_point_flag");
      g.temporal_group_spatial_switching_up_point_flag = b.read_flag("temporal_group_spatial_switching_up_point_flag");
      g.temporal_group_ref_cnt = static_cast<uint8_t>(b.read_u("temporal_group_ref_cnt", 3));
      for (int j = 0; j < g.temporal_group_ref_cnt; ++j) {
        b.begin_array_element("temporal_group_ref_pic_diff", j);
        g.temporal_group_ref_pic_diff.push_back(static_cast<uint8_t>(b.read_u("temporal_group_ref_pic_diff", 8)));
        b.end_array_element();
      }
      b.end_array_element();
      ss.temporal_groups.push_back(std::move(g));
    }
  }
  return ss;
}

MetadataScalability parse_scalability(SyntaxBuilder& b) {
  MetadataScalability m;
  auto s = b.scoped("metadata_scalability", "AV1 5.8.5");
  m.scalability_mode_idc = static_cast<uint8_t>(b.read_u("scalability_mode_idc", 8));
  if (m.scalability_mode_idc == kScalabilitySs) {
    b.set_enum("SCALABILITY_SS");
    m.scalability_structure = parse_scalability_structure(b);
  } else if (m.scalability_mode_idc > kScalabilitySs) {
    // SPEC-ASSUMPTION(#321): AV1 §6.7.5 reserves 15..255; Warning SYN-AV1-0004, parsing continues.
    b.diag(Severity::Warning, "SYN-AV1-0004",
           fmt::format("scalability_mode_idc: reserved value {}", m.scalability_mode_idc), {}, true);
  }
  return m;
}

// Width in bits of the itu_t_t35_terminal_provider_oriented_code the registry consumed for `r`.
unsigned oriented_code_bits(const T35Result& r, uint64_t bits_after_provider) noexcept {
  if (!r.provider_oriented_code || !r.provider_code) {
    return 0;
  }
  for (const T35Entry& e : kT35Registry) {
    if (r.kind == e.kind && e.country_code == r.country_code && e.provider_code == *r.provider_code) {
      return e.oriented_width == OrientedCode::U32 ? 32 : e.oriented_width == OrientedCode::U16 ? 16 : 0;
    }
  }
  return bits_after_provider >= 16 ? 16 : 0;  // UnknownProvider: parse_t35 peeks 16 bits when they exist
}

MetadataItutT35 parse_itut_t35(BitReader& br, SyntaxBuilder& b) {
  MetadataItutT35 m;
  auto s = b.scoped("metadata_itut_t35", "AV1 5.8.2");
  // The whole message (country code to the end of the OBU, trailing bits inside) goes to the registry first;
  // the fields it consumed are then read again through the builder so the tree shows them (§7.3.11).
  const std::span<const uint8_t> message = br.remaining_bytes();
  const T35Result r = parse_t35(message, T35Carrier::Av1Metadata);
  m.itu_t_t35_country_code = static_cast<uint8_t>(b.read_u("itu_t_t35_country_code", 8));
  if (m.itu_t_t35_country_code == 0xFF) {
    m.itu_t_t35_country_code_extension_byte =
        static_cast<uint8_t>(b.read_u("itu_t_t35_country_code_extension_byte", 8));
  }
  if (r.provider_code) {
    m.itu_t_t35_terminal_provider_code = static_cast<uint16_t>(b.read_u("itu_t_t35_terminal_provider_code", 16));
    b.set_enum(r.enum_name);
    const unsigned bits = oriented_code_bits(r, br.bits_left());
    if (bits > 0) {
      m.itu_t_t35_terminal_provider_oriented_code =
          static_cast<uint32_t>(b.read_u("itu_t_t35_terminal_provider_oriented_code", bits));
    }
  }
  m.enum_name = r.enum_name;
  m.has_hdr10plus = r.has_hdr10plus;
  m.has_dolby_vision = r.has_dolby_vision;
  m.t35_message = strip_av1_trailing_bits(message);
  // itu_t_t35_payload_bytes run to the end of the OBU minus the trailing bits (§7.3.6 row 9, Errata 1).
  const size_t payload_bytes = strip_av1_trailing_bits(br.remaining_bytes()).size();
  (void)b.read_bytes("itu_t_t35_payload_bytes", payload_bytes);
  b.derived("t35", fmt::format("enum_name = \"{}\", has_hdr10plus = {}, has_dolby_vision = {}", r.enum_name,
                               r.has_hdr10plus ? 1 : 0, r.has_dolby_vision ? 1 : 0));
  return m;
}

MetadataTimecode parse_timecode(SyntaxBuilder& b) {
  MetadataTimecode t;
  auto s = b.scoped("metadata_timecode", "AV1 5.8.7");
  t.counting_type = static_cast<uint8_t>(b.read_u("counting_type", 5));
  t.full_timestamp_flag = b.read_flag("full_timestamp_flag");
  t.discontinuity_flag = b.read_flag("discontinuity_flag");
  t.cnt_dropped_flag = b.read_flag("cnt_dropped_flag");
  t.n_frames = static_cast<uint16_t>(b.read_u("n_frames", 9));
  if (t.full_timestamp_flag) {
    t.seconds_flag = t.minutes_flag = t.hours_flag = true;
    t.seconds_value = static_cast<uint8_t>(b.read_u("seconds_value", 6));
    t.minutes_value = static_cast<uint8_t>(b.read_u("minutes_value", 6));
    t.hours_value = static_cast<uint8_t>(b.read_u("hours_value", 5));
  } else {
    t.seconds_flag = b.read_flag("seconds_flag");
    if (t.seconds_flag) {
      t.seconds_value = static_cast<uint8_t>(b.read_u("seconds_value", 6));
      t.minutes_flag = b.read_flag("minutes_flag");
      if (t.minutes_flag) {
        t.minutes_value = static_cast<uint8_t>(b.read_u("minutes_value", 6));
        t.hours_flag = b.read_flag("hours_flag");
        if (t.hours_flag) {
          t.hours_value = static_cast<uint8_t>(b.read_u("hours_value", 5));
        }
      }
    }
  }
  t.time_offset_length = static_cast<uint8_t>(b.read_u("time_offset_length", 5));
  if (t.time_offset_length > 0) {
    t.time_offset_value = static_cast<uint32_t>(b.read_u("time_offset_value", t.time_offset_length));
    b.set_descriptor_text("f(n)");
  }
  auto field = [](bool present, unsigned v) { return present ? fmt::format("{:02}", v) : std::string("--"); };
  t.timecode_text =
      fmt::format("{}:{}:{}:{:02}", field(t.hours_flag, t.hours_value), field(t.minutes_flag, t.minutes_value),
                  field(t.seconds_flag, t.seconds_value), t.n_frames);
  b.derived("timecode", fmt::format("timecode = {}", t.timecode_text));
  return t;
}

}  // namespace

Metadata parse_metadata(BitReader& br, SyntaxBuilder& b) {
  Metadata m;
  const auto start = br.mark();
  {
    auto s = b.scoped("metadata_obu", kSpecMetadata);
    m.metadata_type = b.read_leb128("metadata_type");
    b.set_enum(metadata_type_name(m.metadata_type));
    switch (m.metadata_type) {
      case 1:
        m.hdr_cll = parse_hdr_cll(b);
        break;
      case 2:
        m.hdr_mdcv = parse_hdr_mdcv(b);
        break;
      case 3:
        m.scalability = parse_scalability(b);
        break;
      case 4:
        m.itut_t35 = parse_itut_t35(br, b);
        break;
      case 5:
        m.timecode = parse_timecode(b);
        break;
      default: {
        // FR-SYN-012: unknown payloads are kept as bytes, never an error.
        auto u = b.scoped("metadata_unknown");
        b.derived("payload_type", m.metadata_type);
        const size_t n = strip_av1_trailing_bits(br.remaining_bytes()).size();
        b.derived("payload_size", n);
        m.unknown_payload = b.read_bytes("bytes", n);
        // SPEC-ASSUMPTION(#321): Info SYN-AV1-0005 for user private and reserved metadata types.
        b.diag(Severity::Info, "SYN-AV1-0005",
               fmt::format("metadata_type {}: unregistered user private or reserved; payload kept as bytes",
                           m.metadata_type));
        break;
      }
    }
  }
  if (br.bits_left() > 0) {
    parse_trailing_bits(br, b);  // AV1 §5.3.1: every metadata OBU ends with trailing_bits()
  }
  m.span = br.span_since(start);
  return m;
}

void apply_metadata(const Metadata& m, ovt_ColorDescription& color) noexcept {
  if (m.hdr_cll) {
    color.max_cll = m.hdr_cll->max_cll;
    color.max_fall = m.hdr_cll->max_fall;
  }
  if (m.hdr_mdcv) {
    for (size_t i = 0; i < 3; ++i) {
      color.mdcv_primaries[i][0] = m.hdr_mdcv->mdcv_primaries[i][0];
      color.mdcv_primaries[i][1] = m.hdr_mdcv->mdcv_primaries[i][1];
    }
    color.mdcv_white_point[0] = m.hdr_mdcv->mdcv_white_point[0];
    color.mdcv_white_point[1] = m.hdr_mdcv->mdcv_white_point[1];
    color.mdcv_max_luminance = m.hdr_mdcv->mdcv_max_luminance;
    color.mdcv_min_luminance = m.hdr_mdcv->mdcv_min_luminance;
  }
  if (m.itut_t35) {
    if (m.itut_t35->has_hdr10plus) {
      color.has_hdr10plus = 1;
    }
    if (m.itut_t35->has_dolby_vision) {
      color.has_dolby_vision = 1;
    }
  }
}

}  // namespace ovt::syntax::av1
