// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 SEI payloads that describe the picture: frame_packing_arrangement (D.2.16), display_orientation (D.2.17),
// mastering_display_colour_volume (D.2.28), content_light_level_info (D.2.35), alternative_transfer_characteristics
// (D.2.38) and ambient_viewing_environment (D.2.39); chapter 7 §7.3.5 row 14, WI-1.114b. The derived texts match the
// H.264 parsers of §7.3.4 rows 13–17.

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ovt/cxx/syntax/h265/sei.hpp"
#include "syntax/h265/sei_internal.h"

namespace ovt::syntax::h265::detail {

namespace {

// Table D-8.
std::string_view fpa_type_name(uint32_t type) noexcept {
  switch (type) {
    case 0:
      return "checkerboard";
    case 1:
      return "column interleaving";
    case 2:
      return "row interleaving";
    case 3:
      return "side by side";
    case 4:
      return "top bottom";
    case 5:
      return "frame sequential";
    case 6:
      return "2D";
    default:
      return "reserved";
  }
}

// value / unit_denominator as decimal text with at most `decimals` places and no trailing zeros ("1000", "0.0001").
std::string scaled(uint64_t value, uint64_t unit_denominator, unsigned decimals) {
  std::string s = fmt::format("{:.{}f}", static_cast<double>(value) / static_cast<double>(unit_denominator), decimals);
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') {
      s.pop_back();
    }
    if (s.back() == '.') {
      s.pop_back();
    }
  }
  return s;
}

// Chromaticity coordinate in units of 0.00002 with fixed decimals.
std::string chroma(uint32_t value, unsigned decimals) {
  return fmt::format("{:.{}f}", static_cast<double>(value) * 0.00002, decimals);
}

}  // namespace

FramePackingArrangement parse_frame_packing_arrangement(SeiPayloadReader& r) {
  FramePackingArrangement f;
  f.frame_packing_arrangement_id = r.ue("frame_packing_arrangement_id");
  f.cancel_flag = r.flag("frame_packing_arrangement_cancel_flag");
  if (!f.cancel_flag) {
    f.type = static_cast<uint32_t>(r.u("frame_packing_arrangement_type", 7));
    f.type_name = fpa_type_name(f.type);
    r.builder().set_enum(f.type_name);
    f.quincunx_sampling_flag = r.flag("quincunx_sampling_flag");
    f.content_interpretation_type = static_cast<uint32_t>(r.u("content_interpretation_type", 6));
    f.spatial_flipping_flag = r.flag("spatial_flipping_flag");
    f.frame0_flipped_flag = r.flag("frame0_flipped_flag");
    f.field_views_flag = r.flag("field_views_flag");
    f.current_frame_is_frame0_flag = r.flag("current_frame_is_frame0_flag");
    f.frame0_self_contained_flag = r.flag("frame0_self_contained_flag");
    f.frame1_self_contained_flag = r.flag("frame1_self_contained_flag");
    if (!f.quincunx_sampling_flag && f.type != 5) {
      f.grid_position[0] = static_cast<uint32_t>(r.u("frame0_grid_position_x", 4));
      f.grid_position[1] = static_cast<uint32_t>(r.u("frame0_grid_position_y", 4));
      f.grid_position[2] = static_cast<uint32_t>(r.u("frame1_grid_position_x", 4));
      f.grid_position[3] = static_cast<uint32_t>(r.u("frame1_grid_position_y", 4));
    }
    f.reserved_byte = static_cast<uint32_t>(r.u("frame_packing_arrangement_reserved_byte", 8));
    f.persistence_flag = r.flag("frame_packing_arrangement_persistence_flag");
  }
  f.upsampled_aspect_ratio_flag = r.flag("upsampled_aspect_ratio_flag");
  return f;
}

DisplayOrientation parse_display_orientation(SeiPayloadReader& r) {
  DisplayOrientation d;
  d.cancel_flag = r.flag("display_orientation_cancel_flag");
  if (!d.cancel_flag) {
    d.hor_flip = r.flag("hor_flip");
    d.ver_flip = r.flag("ver_flip");
    d.anticlockwise_rotation = static_cast<uint32_t>(r.u("anticlockwise_rotation", 16));
    d.degrees = static_cast<double>(d.anticlockwise_rotation) * 360.0 / 65536.0;
    r.builder().derived("rotation_degrees", fmt::format("{} degrees anticlockwise", d.degrees));
    d.persistence_flag = r.flag("display_orientation_persistence_flag");
  }
  return d;
}

MasteringDisplayColourVolume parse_mastering_display_colour_volume(SeiPayloadReader& r) {
  MasteringDisplayColourVolume m;
  for (int c = 0; c < 3; ++c) {
    const SeiArrayElement element(r.builder(), "display_primaries", c);
    m.display_primaries_x[static_cast<size_t>(c)] = static_cast<uint32_t>(r.u("display_primaries_x", 16));
    m.display_primaries_y[static_cast<size_t>(c)] = static_cast<uint32_t>(r.u("display_primaries_y", 16));
  }
  m.white_point_x = static_cast<uint32_t>(r.u("white_point_x", 16));
  m.white_point_y = static_cast<uint32_t>(r.u("white_point_y", 16));
  m.max_display_mastering_luminance = static_cast<uint32_t>(r.u("max_display_mastering_luminance", 32));
  m.min_display_mastering_luminance = static_cast<uint32_t>(r.u("min_display_mastering_luminance", 32));
  // The SEI carries G, B, R (D.3.28); ovt_ColorDescription.mdcv_primaries is R, G, B (§7.3.4 row 15).
  constexpr std::array<size_t, 3> kRgbFromCoded{2, 0, 1};
  for (size_t i = 0; i < 3; ++i) {
    m.mdcv_primaries_x[i] = m.display_primaries_x[kRgbFromCoded[i]];
    m.mdcv_primaries_y[i] = m.display_primaries_y[kRgbFromCoded[i]];
  }
  m.text = fmt::format("R ({}, {}) G ({}, {}) B ({}, {}) WP ({}, {}) max {} cd/m² min {} cd/m²",
                       chroma(m.mdcv_primaries_x[0], 3), chroma(m.mdcv_primaries_y[0], 3),
                       chroma(m.mdcv_primaries_x[1], 3), chroma(m.mdcv_primaries_y[1], 3),
                       chroma(m.mdcv_primaries_x[2], 3), chroma(m.mdcv_primaries_y[2], 3), chroma(m.white_point_x, 4),
                       chroma(m.white_point_y, 4), scaled(m.max_display_mastering_luminance, 10000, 4),
                       scaled(m.min_display_mastering_luminance, 10000, 4));
  r.builder().derived("mastering_display", m.text);
  return m;
}

ContentLightLevelInfo parse_content_light_level_info(SeiPayloadReader& r) {
  ContentLightLevelInfo c;
  c.max_content_light_level = static_cast<uint32_t>(r.u("max_content_light_level", 16));
  c.max_pic_average_light_level = static_cast<uint32_t>(r.u("max_pic_average_light_level", 16));
  r.builder().derived("content_light_level", fmt::format("MaxCLL {} cd/m², MaxFALL {} cd/m²", c.max_content_light_level,
                                                         c.max_pic_average_light_level));
  return c;
}

AlternativeTransferCharacteristics parse_alternative_transfer_characteristics(SeiPayloadReader& r) {
  AlternativeTransferCharacteristics a;
  a.preferred_transfer_characteristics = static_cast<uint32_t>(r.u("preferred_transfer_characteristics", 8));
  return a;
}

AmbientViewingEnvironment parse_ambient_viewing_environment(SeiPayloadReader& r) {
  AmbientViewingEnvironment a;
  a.ambient_illuminance = static_cast<uint32_t>(r.u("ambient_illuminance", 32));
  a.ambient_light_x = static_cast<uint32_t>(r.u("ambient_light_x", 16));
  a.ambient_light_y = static_cast<uint32_t>(r.u("ambient_light_y", 16));
  r.builder().derived("ambient_viewing_environment",
                      fmt::format("{} lux, x {} y {}", scaled(a.ambient_illuminance, 10000, 4),
                                  chroma(a.ambient_light_x, 4), chroma(a.ambient_light_y, 4)));
  return a;
}

}  // namespace ovt::syntax::h265::detail
