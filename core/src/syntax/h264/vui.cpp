// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 vui_parameters() (Annex E.1.1) and hrd_parameters() (E.1.2) with their derived values (chapter 7
// §7.3.4 rows 4 and 5, WI-1.108). Element order follows H.264 (08/2021) Annex E line by line.

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/h273_names.h"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

std::string_view aspect_ratio_name(uint32_t idc) noexcept {
  switch (idc) {  // Table E-1
    case 0:
      return "Unspecified";
    case 1:
      return "1:1";
    case 2:
      return "12:11";
    case 3:
      return "10:11";
    case 4:
      return "16:11";
    case 5:
      return "40:33";
    case 6:
      return "24:11";
    case 7:
      return "20:11";
    case 8:
      return "32:11";
    case 9:
      return "80:33";
    case 10:
      return "18:11";
    case 11:
      return "15:11";
    case 12:
      return "64:33";
    case 13:
      return "160:99";
    case 14:
      return "4:3";
    case 15:
      return "3:2";
    case 16:
      return "2:1";
    case 255:
      return "Extended_SAR";
    default:
      return {};
  }
}

std::string_view video_format_name(uint32_t v) noexcept {
  switch (v) {  // Table E-2
    case 0:
      return "Component";
    case 1:
      return "PAL";
    case 2:
      return "NTSC";
    case 3:
      return "SECAM";
    case 4:
      return "MAC";
    case 5:
      return "Unspecified";
    default:
      return {};
  }
}

void enum_or_warn(SyntaxBuilder& b, std::string_view name, uint32_t value, std::string_view enum_name) {
  if (enum_name.empty()) {
    detail::warn_reserved(b, name, value, "H.264 E.2.1");
  } else {
    b.set_enum(enum_name);
  }
}

// Figure E-1 chroma sample location → ovt_ColorDescription.chroma_siting (row 4).
uint8_t chroma_siting_code(uint32_t loc_type) noexcept {
  switch (loc_type) {
    case 0:
      return 1;  // left
    case 1:
      return 2;  // centre
    case 2:
      return 3;  // top-left
    default:
      return 0;  // 3..5 have no OVT siting code
  }
}

}  // namespace

HrdParameters parse_hrd_parameters(BitReader& br, SyntaxBuilder& b) {
  HrdParameters h;
  const auto start = br.mark();
  auto s = b.scoped("hrd_parameters", "H.264 E.1.2");
  h.cpb_cnt_minus1 = b.read_ue("cpb_cnt_minus1");
  detail::require_range(br, "cpb_cnt_minus1", h.cpb_cnt_minus1, 0, 31);
  h.bit_rate_scale = static_cast<uint32_t>(b.read_u("bit_rate_scale", 4));
  h.cpb_size_scale = static_cast<uint32_t>(b.read_u("cpb_size_scale", 4));
  const uint32_t n = h.cpb_cnt_minus1 + 1;
  h.bit_rate_value_minus1.resize(n);
  h.cpb_size_value_minus1.resize(n);
  h.cbr_flag.resize(n);
  h.BitRate.resize(n);
  h.CpbSize.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    b.begin_array_element("SchedSelIdx", static_cast<int>(i));
    h.bit_rate_value_minus1[i] = b.read_ue("bit_rate_value_minus1");
    h.cpb_size_value_minus1[i] = b.read_ue("cpb_size_value_minus1");
    h.cbr_flag[i] = b.read_flag("cbr_flag");
    h.BitRate[i] = (uint64_t{h.bit_rate_value_minus1[i]} + 1) << (6U + h.bit_rate_scale);  // E-37
    h.CpbSize[i] = (uint64_t{h.cpb_size_value_minus1[i]} + 1) << (4U + h.cpb_size_scale);  // E-38
    b.derived("BitRate", h.BitRate[i]);
    b.derived("CpbSize", h.CpbSize[i]);
    b.end_array_element();
  }
  h.initial_cpb_removal_delay_length_minus1 =
      static_cast<uint32_t>(b.read_u("initial_cpb_removal_delay_length_minus1", 5));
  h.cpb_removal_delay_length_minus1 = static_cast<uint32_t>(b.read_u("cpb_removal_delay_length_minus1", 5));
  h.dpb_output_delay_length_minus1 = static_cast<uint32_t>(b.read_u("dpb_output_delay_length_minus1", 5));
  h.time_offset_length = static_cast<uint32_t>(b.read_u("time_offset_length", 5));
  h.span = br.span_since(start);
  return h;
}

VuiParameters parse_vui_parameters(BitReader& br, SyntaxBuilder& b) {
  VuiParameters v;
  const auto start = br.mark();
  auto s = b.scoped("vui_parameters", "H.264 E.1.1");
  v.aspect_ratio_info_present_flag = b.read_flag("aspect_ratio_info_present_flag");
  if (v.aspect_ratio_info_present_flag) {
    v.aspect_ratio_idc = static_cast<uint32_t>(b.read_u("aspect_ratio_idc", 8));
    enum_or_warn(b, "aspect_ratio_idc", v.aspect_ratio_idc, aspect_ratio_name(v.aspect_ratio_idc));
    if (v.aspect_ratio_idc == 255) {  // Extended_SAR
      v.sar_width = static_cast<uint32_t>(b.read_u("sar_width", 16));
      v.sar_height = static_cast<uint32_t>(b.read_u("sar_height", 16));
    }
  }
  v.overscan_info_present_flag = b.read_flag("overscan_info_present_flag");
  if (v.overscan_info_present_flag) {
    v.overscan_appropriate_flag = b.read_flag("overscan_appropriate_flag");
  }
  v.video_signal_type_present_flag = b.read_flag("video_signal_type_present_flag");
  if (v.video_signal_type_present_flag) {
    v.video_format = static_cast<uint32_t>(b.read_u("video_format", 3));
    enum_or_warn(b, "video_format", v.video_format, video_format_name(v.video_format));
    v.video_full_range_flag = b.read_flag("video_full_range_flag");
    v.colour_description_present_flag = b.read_flag("colour_description_present_flag");
    if (v.colour_description_present_flag) {
      v.colour_primaries = static_cast<uint32_t>(b.read_u("colour_primaries", 8));
      enum_or_warn(b, "colour_primaries", v.colour_primaries, h273_colour_primaries_name(v.colour_primaries));
      v.transfer_characteristics = static_cast<uint32_t>(b.read_u("transfer_characteristics", 8));
      enum_or_warn(b, "transfer_characteristics", v.transfer_characteristics,
                   h273_transfer_characteristics_name(v.transfer_characteristics));
      v.matrix_coefficients = static_cast<uint32_t>(b.read_u("matrix_coefficients", 8));
      enum_or_warn(b, "matrix_coefficients", v.matrix_coefficients,
                   h273_matrix_coefficients_name(v.matrix_coefficients));
    }
  }
  v.chroma_loc_info_present_flag = b.read_flag("chroma_loc_info_present_flag");
  if (v.chroma_loc_info_present_flag) {
    v.chroma_sample_loc_type_top_field = b.read_ue("chroma_sample_loc_type_top_field");
    detail::require_range(br, "chroma_sample_loc_type_top_field", v.chroma_sample_loc_type_top_field, 0, 5);
    v.chroma_sample_loc_type_bottom_field = b.read_ue("chroma_sample_loc_type_bottom_field");
    detail::require_range(br, "chroma_sample_loc_type_bottom_field", v.chroma_sample_loc_type_bottom_field, 0, 5);
  }
  v.chroma_siting = chroma_siting_code(v.chroma_sample_loc_type_top_field);
  v.timing_info_present_flag = b.read_flag("timing_info_present_flag");
  if (v.timing_info_present_flag) {
    v.num_units_in_tick = static_cast<uint32_t>(b.read_u("num_units_in_tick", 32));
    v.time_scale = static_cast<uint32_t>(b.read_u("time_scale", 32));
    v.fixed_frame_rate_flag = b.read_flag("fixed_frame_rate_flag");
    if (v.num_units_in_tick > 0) {
      v.frame_rate = static_cast<double>(v.time_scale) / (2.0 * static_cast<double>(v.num_units_in_tick));
      b.derived("frame_rate", std::string_view{fmt::format("frame_rate = {:g} fps", v.frame_rate)});
    } else {
      detail::warn_reserved(b, "num_units_in_tick", v.num_units_in_tick, "H.264 E.2.1");
    }
  }
  v.nal_hrd_parameters_present_flag = b.read_flag("nal_hrd_parameters_present_flag");
  if (v.nal_hrd_parameters_present_flag) {
    v.nal_hrd = parse_hrd_parameters(br, b);
  }
  v.vcl_hrd_parameters_present_flag = b.read_flag("vcl_hrd_parameters_present_flag");
  if (v.vcl_hrd_parameters_present_flag) {
    v.vcl_hrd = parse_hrd_parameters(br, b);
  }
  v.CpbDpbDelaysPresentFlag = v.nal_hrd_parameters_present_flag || v.vcl_hrd_parameters_present_flag;
  if (v.CpbDpbDelaysPresentFlag) {
    v.low_delay_hrd_flag = b.read_flag("low_delay_hrd_flag");
  }
  v.pic_struct_present_flag = b.read_flag("pic_struct_present_flag");
  v.bitstream_restriction_flag = b.read_flag("bitstream_restriction_flag");
  if (v.bitstream_restriction_flag) {
    v.motion_vectors_over_pic_boundaries_flag = b.read_flag("motion_vectors_over_pic_boundaries_flag");
    v.max_bytes_per_pic_denom = b.read_ue("max_bytes_per_pic_denom");
    detail::require_range(br, "max_bytes_per_pic_denom", v.max_bytes_per_pic_denom, 0, 16);
    v.max_bits_per_mb_denom = b.read_ue("max_bits_per_mb_denom");
    detail::require_range(br, "max_bits_per_mb_denom", v.max_bits_per_mb_denom, 0, 16);
    v.log2_max_mv_length_horizontal = b.read_ue("log2_max_mv_length_horizontal");
    detail::require_range(br, "log2_max_mv_length_horizontal", v.log2_max_mv_length_horizontal, 0, 15);
    v.log2_max_mv_length_vertical = b.read_ue("log2_max_mv_length_vertical");
    detail::require_range(br, "log2_max_mv_length_vertical", v.log2_max_mv_length_vertical, 0, 15);
    v.max_num_reorder_frames = b.read_ue("max_num_reorder_frames");
    detail::require_range(br, "max_num_reorder_frames", v.max_num_reorder_frames, 0, 16);
    v.max_dec_frame_buffering = b.read_ue("max_dec_frame_buffering");
    detail::require_range(br, "max_dec_frame_buffering", v.max_dec_frame_buffering, 0, 16);
  }
  b.derived("CpbDpbDelaysPresentFlag", v.CpbDpbDelaysPresentFlag ? 1 : 0);
  v.span = br.span_since(start);
  return v;
}

}  // namespace ovt::syntax::h264
