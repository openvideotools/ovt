// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 vui_parameters() parser (H.265 E.2.1, E.3.1; H.273 code point names; chapter 7 §7.3.5 row 7, WI-1.112).

#include "ovt/cxx/syntax/h265/vui.hpp"

#include <array>
#include <cstdint>
#include <string_view>

#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

constexpr std::array<std::string_view, 23> kPrimaries = {
    "reserved", "BT.709",   "unspecified", "reserved", "BT.470M",  "BT.470BG", "SMPTE 170M", "SMPTE 240M",
    "film",     "BT.2020",  "SMPTE 428",   "P3-DCI",   "P3-D65",   "reserved", "reserved",   "reserved",
    "reserved", "reserved", "reserved",    "reserved", "reserved", "reserved", "EBU 3213-E"};

constexpr std::array<std::string_view, 19> kTransfer = {
    "reserved", "BT.709",    "unspecified", "reserved",      "BT.470M", "BT.470BG", "SMPTE 170M",  "SMPTE 240M",
    "linear",   "log 100:1", "log 316:1",   "IEC 61966-2-4", "BT.1361", "sRGB",     "BT.2020 10b", "BT.2020 12b",
    "PQ",       "SMPTE 428", "HLG"};

constexpr std::array<std::string_view, 15> kMatrix = {"identity",
                                                      "BT.709",
                                                      "unspecified",
                                                      "reserved",
                                                      "FCC",
                                                      "BT.470BG",
                                                      "SMPTE 170M",
                                                      "SMPTE 240M",
                                                      "YCgCo",
                                                      "BT.2020-NCL",
                                                      "BT.2020-CL",
                                                      "SMPTE 2085",
                                                      "chromaticity-derived NCL",
                                                      "chromaticity-derived CL",
                                                      "ICtCp"};

template <size_t N>
std::string_view name_of(const std::array<std::string_view, N>& table, uint8_t code) noexcept {
  return code < table.size() ? table[code] : std::string_view{"reserved"};
}

void read_video_signal_type(SyntaxBuilder& b, VuiParameters& v) {
  v.video_format = static_cast<uint8_t>(b.read_u("video_format", 3));
  v.video_full_range_flag = b.read_flag("video_full_range_flag");
  v.colour_description_present_flag = b.read_flag("colour_description_present_flag");
  if (v.colour_description_present_flag) {
    v.colour_primaries = static_cast<uint8_t>(b.read_u("colour_primaries", 8));
    b.set_enum(colour_primaries_name(v.colour_primaries));
    v.transfer_characteristics = static_cast<uint8_t>(b.read_u("transfer_characteristics", 8));
    b.set_enum(transfer_characteristics_name(v.transfer_characteristics));
    v.matrix_coeffs = static_cast<uint8_t>(b.read_u("matrix_coeffs", 8));
    b.set_enum(matrix_coeffs_name(v.matrix_coeffs));
  }
}

void read_timing(BitReader& br, SyntaxBuilder& b, VuiParameters& v, unsigned sps_max_sub_layers_minus1) {
  v.vui_num_units_in_tick = static_cast<uint32_t>(b.read_u("vui_num_units_in_tick", 32));
  v.vui_time_scale = static_cast<uint32_t>(b.read_u("vui_time_scale", 32));
  v.vui_poc_proportional_to_timing_flag = b.read_flag("vui_poc_proportional_to_timing_flag");
  if (v.vui_poc_proportional_to_timing_flag) {
    v.vui_num_ticks_poc_diff_one_minus1 = b.read_ue("vui_num_ticks_poc_diff_one_minus1");
  }
  v.vui_hrd_parameters_present_flag = b.read_flag("vui_hrd_parameters_present_flag");
  if (v.vui_hrd_parameters_present_flag) {
    v.hrd_parameters = parse_hrd_parameters(br, b, true, sps_max_sub_layers_minus1);
  }
  if (v.vui_num_units_in_tick > 0) {
    v.frame_rate = static_cast<double>(v.vui_time_scale) / static_cast<double>(v.vui_num_units_in_tick);
    b.derived("frame_rate", std::string_view{fmt::format("frame_rate = {:g}", v.frame_rate)});
  } else {
    b.diag(Severity::Error, "SYN-H265-0007",
           "vui_num_units_in_tick out of range (vui_num_units_in_tick = 0, allowed > 0)", "H.265 E.3.1");
  }
}

void read_bitstream_restriction(SyntaxBuilder& b, VuiParameters& v) {
  v.tiles_fixed_structure_flag = b.read_flag("tiles_fixed_structure_flag");
  v.motion_vectors_over_pic_boundaries_flag = b.read_flag("motion_vectors_over_pic_boundaries_flag");
  v.restricted_ref_pic_lists_flag = b.read_flag("restricted_ref_pic_lists_flag");
  v.min_spatial_segmentation_idc = b.read_ue("min_spatial_segmentation_idc");
  detail::check_range(b, "min_spatial_segmentation_idc", v.min_spatial_segmentation_idc, 0, 4095, "H.265 E.3.1");
  v.max_bytes_per_pic_denom = b.read_ue("max_bytes_per_pic_denom");
  detail::check_range(b, "max_bytes_per_pic_denom", v.max_bytes_per_pic_denom, 0, 16, "H.265 E.3.1");
  v.max_bits_per_min_cu_denom = b.read_ue("max_bits_per_min_cu_denom");
  detail::check_range(b, "max_bits_per_min_cu_denom", v.max_bits_per_min_cu_denom, 0, 16, "H.265 E.3.1");
  v.log2_max_mv_length_horizontal = b.read_ue("log2_max_mv_length_horizontal");
  detail::check_range(b, "log2_max_mv_length_horizontal", v.log2_max_mv_length_horizontal, 0, 15, "H.265 E.3.1");
  v.log2_max_mv_length_vertical = b.read_ue("log2_max_mv_length_vertical");
  detail::check_range(b, "log2_max_mv_length_vertical", v.log2_max_mv_length_vertical, 0, 15, "H.265 E.3.1");
}

// §7.3.5 row 7: chroma_sample_loc_type_top_field 0 → 1 (left), 1 → 2 (centre), 2 → 3 (top-left), else 0; the HEVC
// default (no chroma_loc_info, 4:2:0) is top-left.
uint8_t chroma_siting(const VuiParameters& v, unsigned chroma_array_type) {
  if (!v.chroma_loc_info_present_flag) {
    return chroma_array_type == 1 ? 3 : 0;
  }
  switch (v.chroma_sample_loc_type_top_field) {
    case 0:
      return 1;
    case 1:
      return 2;
    case 2:
      return 3;
    default:
      return 0;
  }
}

}  // namespace

std::string_view colour_primaries_name(uint8_t code) noexcept {
  return name_of(kPrimaries, code);
}
std::string_view transfer_characteristics_name(uint8_t code) noexcept {
  return name_of(kTransfer, code);
}
std::string_view matrix_coeffs_name(uint8_t code) noexcept {
  return name_of(kMatrix, code);
}

VuiParameters parse_vui_parameters(BitReader& br, SyntaxBuilder& b, unsigned sps_max_sub_layers_minus1,
                                   unsigned chroma_array_type) {
  VuiParameters v;
  const uint64_t start = br.bit_pos();
  auto s = b.scoped("vui_parameters", "H.265 E.2.1");
  v.aspect_ratio_info_present_flag = b.read_flag("aspect_ratio_info_present_flag");
  if (v.aspect_ratio_info_present_flag) {
    v.aspect_ratio_idc = static_cast<uint8_t>(b.read_u("aspect_ratio_idc", 8));
    if (v.aspect_ratio_idc == 255) {
      b.set_enum("EXTENDED_SAR");
      v.sar_width = static_cast<uint16_t>(b.read_u("sar_width", 16));
      v.sar_height = static_cast<uint16_t>(b.read_u("sar_height", 16));
    }
  }
  v.overscan_info_present_flag = b.read_flag("overscan_info_present_flag");
  if (v.overscan_info_present_flag) {
    v.overscan_appropriate_flag = b.read_flag("overscan_appropriate_flag");
  }
  v.video_signal_type_present_flag = b.read_flag("video_signal_type_present_flag");
  if (v.video_signal_type_present_flag) {
    read_video_signal_type(b, v);
  }
  v.chroma_loc_info_present_flag = b.read_flag("chroma_loc_info_present_flag");
  if (v.chroma_loc_info_present_flag) {
    v.chroma_sample_loc_type_top_field = b.read_ue("chroma_sample_loc_type_top_field");
    detail::check_range(b, "chroma_sample_loc_type_top_field", v.chroma_sample_loc_type_top_field, 0, 5, "H.265 E.3.1");
    v.chroma_sample_loc_type_bottom_field = b.read_ue("chroma_sample_loc_type_bottom_field");
    detail::check_range(b, "chroma_sample_loc_type_bottom_field", v.chroma_sample_loc_type_bottom_field, 0, 5,
                        "H.265 E.3.1");
  }
  v.neutral_chroma_indication_flag = b.read_flag("neutral_chroma_indication_flag");
  v.field_seq_flag = b.read_flag("field_seq_flag");
  v.frame_field_info_present_flag = b.read_flag("frame_field_info_present_flag");
  v.default_display_window_flag = b.read_flag("default_display_window_flag");
  if (v.default_display_window_flag) {
    v.def_disp_win_left_offset = b.read_ue("def_disp_win_left_offset");
    v.def_disp_win_right_offset = b.read_ue("def_disp_win_right_offset");
    v.def_disp_win_top_offset = b.read_ue("def_disp_win_top_offset");
    v.def_disp_win_bottom_offset = b.read_ue("def_disp_win_bottom_offset");
  }
  v.vui_timing_info_present_flag = b.read_flag("vui_timing_info_present_flag");
  if (v.vui_timing_info_present_flag) {
    read_timing(br, b, v, sps_max_sub_layers_minus1);
  }
  v.bitstream_restriction_flag = b.read_flag("bitstream_restriction_flag");
  if (v.bitstream_restriction_flag) {
    read_bitstream_restriction(b, v);
  }
  v.color.struct_size = sizeof(ovt_ColorDescription);
  v.color.primaries = v.colour_primaries;
  v.color.transfer = v.transfer_characteristics;
  v.color.matrix = v.matrix_coeffs;
  v.color.full_range = v.video_full_range_flag ? 1 : 0;
  v.color.chroma_siting = chroma_siting(v, chroma_array_type);
  b.derived("ColorDescription",
            std::string_view{fmt::format(
                "primaries {} transfer {} matrix {} {} range siting {}", colour_primaries_name(v.colour_primaries),
                transfer_characteristics_name(v.transfer_characteristics), matrix_coeffs_name(v.matrix_coeffs),
                v.video_full_range_flag ? "full" : "limited", v.color.chroma_siting)});
  v.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return v;
}

}  // namespace ovt::syntax::h265
