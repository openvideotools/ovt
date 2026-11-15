// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 SEI payloads that describe the picture: recovery_point (D.1.8), frame_packing_arrangement (D.1.26),
// display_orientation (D.1.27), film_grain_characteristics (D.1.20), tone_mapping_info (D.1.21),
// mastering_display_colour_volume (D.1.29), content_light_level_info (D.1.30) and
// alternative_transfer_characteristics (D.1.31); chapter 7 §7.3.4 rows 10 and 13–18, WI-1.110.

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "ovt/cxx/syntax/h264/sei.hpp"
#include "syntax/h264/h264_internal.h"
#include "syntax/h264/sei_internal.h"

namespace ovt::syntax::h264::detail {

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

// value * unit as decimal text with at most `decimals` places and no trailing zeros ("1000", "0.0001").
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

// Fixed decimals, as the chromaticity coordinates are printed in the tests and by `--master-display` tools.
std::string chroma(uint32_t value, unsigned decimals) {
  return fmt::format("{:.{}f}", static_cast<double>(value) * 0.00002, decimals);
}

}  // namespace

RecoveryPoint parse_recovery_point(SeiPayloadReader& r) {
  RecoveryPoint p;
  p.recovery_frame_cnt = r.ue("recovery_frame_cnt");
  p.exact_match_flag = r.flag("exact_match_flag");
  p.broken_link_flag = r.flag("broken_link_flag");
  p.changing_slice_group_idc = static_cast<uint32_t>(r.u("changing_slice_group_idc", 2));
  return p;
}

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
    f.repetition_period = r.ue("frame_packing_arrangement_repetition_period");
  }
  f.extension_flag = r.flag("frame_packing_arrangement_extension_flag");
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
    d.repetition_period = r.ue("display_orientation_repetition_period");
    d.extension_flag = r.flag("display_orientation_extension_flag");
  }
  return d;
}

MasteringDisplayColourVolume parse_mastering_display_colour_volume(SeiPayloadReader& r) {
  MasteringDisplayColourVolume m;
  for (int c = 0; c < 3; ++c) {
    const ArrayElementScope element(r.builder(), "display_primaries", c);
    m.display_primaries_x[static_cast<size_t>(c)] = static_cast<uint32_t>(r.u("display_primaries_x", 16));
    m.display_primaries_y[static_cast<size_t>(c)] = static_cast<uint32_t>(r.u("display_primaries_y", 16));
  }
  m.white_point_x = static_cast<uint32_t>(r.u("white_point_x", 16));
  m.white_point_y = static_cast<uint32_t>(r.u("white_point_y", 16));
  m.max_display_mastering_luminance = static_cast<uint32_t>(r.u("max_display_mastering_luminance", 32));
  m.min_display_mastering_luminance = static_cast<uint32_t>(r.u("min_display_mastering_luminance", 32));
  // The SEI carries G, B, R; ovt_ColorDescription.mdcv_primaries is R, G, B (§7.3.4 row 15).
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

FilmGrainCharacteristics parse_film_grain_characteristics(SeiPayloadReader& r) {
  FilmGrainCharacteristics f;
  f.cancel_flag = r.flag("film_grain_characteristics_cancel_flag");
  if (f.cancel_flag) {
    return f;
  }
  f.film_grain_model_id = static_cast<uint32_t>(r.u("film_grain_model_id", 2));
  f.separate_colour_description_present_flag = r.flag("separate_colour_description_present_flag");
  if (f.separate_colour_description_present_flag) {
    f.bit_depth_luma_minus8 = static_cast<uint32_t>(r.u("film_grain_bit_depth_luma_minus8", 3));
    f.bit_depth_chroma_minus8 = static_cast<uint32_t>(r.u("film_grain_bit_depth_chroma_minus8", 3));
    f.full_range_flag = r.flag("film_grain_full_range_flag");
    f.colour_primaries = static_cast<uint32_t>(r.u("film_grain_colour_primaries", 8));
    f.transfer_characteristics = static_cast<uint32_t>(r.u("film_grain_transfer_characteristics", 8));
    f.matrix_coefficients = static_cast<uint32_t>(r.u("film_grain_matrix_coefficients", 8));
  }
  f.blending_mode_id = static_cast<uint32_t>(r.u("blending_mode_id", 2));
  f.log2_scale_factor = static_cast<uint32_t>(r.u("log2_scale_factor", 4));
  for (int c = 0; c < 3; ++c) {
    const ArrayElementScope element(r.builder(), "comp_model_present_flag", c);
    f.comp_model_present_flag[static_cast<size_t>(c)] = r.flag("comp_model_present_flag");
  }
  for (int c = 0; c < 3; ++c) {
    if (!f.comp_model_present_flag[static_cast<size_t>(c)]) {
      continue;
    }
    FilmGrainComponent& comp = f.comp[static_cast<size_t>(c)];
    const ArrayElementScope element(r.builder(), "comp_model", c);
    comp.num_intensity_intervals_minus1 = static_cast<uint32_t>(r.u("num_intensity_intervals_minus1", 8));
    comp.num_model_values_minus1 = static_cast<uint32_t>(r.u("num_model_values_minus1", 3));
    for (uint32_t i = 0; i <= comp.num_intensity_intervals_minus1; ++i) {
      const ArrayElementScope interval(r.builder(), "intensity_interval", static_cast<int>(i));
      comp.intensity_interval_lower_bound.push_back(static_cast<uint32_t>(r.u("intensity_interval_lower_bound", 8)));
      comp.intensity_interval_upper_bound.push_back(static_cast<uint32_t>(r.u("intensity_interval_upper_bound", 8)));
      for (uint32_t j = 0; j <= comp.num_model_values_minus1; ++j) {
        comp.comp_model_value.push_back(r.se("comp_model_value"));
      }
    }
  }
  f.repetition_period = r.ue("film_grain_characteristics_repetition_period");
  return f;
}

ToneMappingInfo parse_tone_mapping_info(SeiPayloadReader& r) {
  ToneMappingInfo t;
  t.tone_map_id = r.ue("tone_map_id");
  t.cancel_flag = r.flag("tone_map_cancel_flag");
  if (t.cancel_flag) {
    return t;
  }
  t.repetition_period = r.ue("tone_map_repetition_period");
  t.coded_data_bit_depth = static_cast<uint32_t>(r.u("coded_data_bit_depth", 8));
  t.target_bit_depth = static_cast<uint32_t>(r.u("target_bit_depth", 8));
  require_range(r.reader(), "target_bit_depth", t.target_bit_depth, 1, 16);
  t.tone_map_model_id = r.ue("tone_map_model_id");
  switch (t.tone_map_model_id) {
    case 0:
      t.values.push_back(static_cast<uint32_t>(r.u("min_value", 32)));
      t.values.push_back(static_cast<uint32_t>(r.u("max_value", 32)));
      break;
    case 1:
      t.values.push_back(static_cast<uint32_t>(r.u("sigmoid_midpoint", 32)));
      t.values.push_back(static_cast<uint32_t>(r.u("sigmoid_width", 32)));
      break;
    case 2: {
      const unsigned bits = ((t.coded_data_bit_depth + 7U) >> 3U) << 3U;
      for (uint32_t i = 0; i < (1U << t.target_bit_depth); ++i) {
        const ArrayElementScope element(r.builder(), "start_of_coded_interval", static_cast<int>(i));
        t.values.push_back(static_cast<uint32_t>(r.u("start_of_coded_interval", bits)));
        r.builder().set_descriptor_text("u(v)");
      }
      break;
    }
    case 3: {
      const auto num_pivots = static_cast<uint32_t>(r.u("num_pivots", 16));
      const unsigned coded_bits = ((t.coded_data_bit_depth + 7U) >> 3U) << 3U;
      const unsigned target_bits = ((t.target_bit_depth + 7U) >> 3U) << 3U;
      for (uint32_t i = 0; i < num_pivots; ++i) {
        const ArrayElementScope element(r.builder(), "pivot", static_cast<int>(i));
        t.values.push_back(static_cast<uint32_t>(r.u("coded_pivot_value", coded_bits)));
        r.builder().set_descriptor_text("u(v)");
        t.values.push_back(static_cast<uint32_t>(r.u("target_pivot_value", target_bits)));
        r.builder().set_descriptor_text("u(v)");
      }
      break;
    }
    case 4: {
      constexpr uint32_t kExtendedIso = 255;
      const auto iso_idc = static_cast<uint32_t>(r.u("camera_iso_speed_idc", 8));
      if (iso_idc == kExtendedIso) {
        (void)r.u("camera_iso_speed_value", 32);
      }
      const auto exposure_idc = static_cast<uint32_t>(r.u("exposure_index_idc", 8));
      if (exposure_idc == kExtendedIso) {
        (void)r.u("exposure_index_value", 32);
      }
      if (r.flag("exposure_compensation_signalled_flag")) {
        (void)r.u("exposure_compensation_numerator_idc", 16);
        (void)r.u("exposure_compensation_denominator_idc", 8);
      }
      (void)r.u("ref_screen_luminance_white", 32);
      (void)r.u("extended_range_white_level", 32);
      (void)r.u("nominal_black_level_code_value", 16);
      (void)r.u("nominal_white_level_code_value", 16);
      (void)r.u("extended_white_level_code_value", 16);
      break;
    }
    default:
      warn_reserved(r.builder(), "tone_map_model_id", t.tone_map_model_id, "H.264 D.2.21");
      break;
  }
  return t;
}

}  // namespace ovt::syntax::h264::detail
