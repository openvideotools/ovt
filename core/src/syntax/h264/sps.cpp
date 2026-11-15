// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 seq_parameter_set_data() / seq_parameter_set_rbsp() with the derived table of §7.4.2.1.1 and Table A-1
// (chapter 7 §7.3.4 row 2, WI-1.108). Element order follows H.264 (08/2021) §7.3.2.1.1 line by line.

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "ovt/color.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

// SPEC-ASSUMPTION(#388): arithmetic-safety bound only, mirroring the H.265 kMaxLumaDimension cap. With 16-bit
// macroblock dimensions FrameHeightInMbs stays in uint32 and PicSizeInMapUnits / PicWidthInMbs*FrameHeightInMbs
// (uint64) are well defined; the level limit (Table A-1 MaxFS) is the conformance Warning of WI-4.109.
constexpr uint32_t kMaxPicDimensionMinus1 = 65535;

// profile_idc values whose SPS carries the chroma_format_idc … seq_scaling_matrix_present_flag block (§7.3.2.1.1).
bool has_high_profile_syntax(uint32_t profile_idc) noexcept {
  switch (profile_idc) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135:
      return true;
    default:
      return false;
  }
}

std::string_view chroma_format_name(uint32_t idc) noexcept {
  switch (idc) {  // Table 6-1
    case 0:
      return "4:0:0";
    case 1:
      return "4:2:0";
    case 2:
      return "4:2:2";
    case 3:
      return "4:4:4";
    default:
      return {};
  }
}

void derive(Sps& s) {
  s.ChromaArrayType = s.separate_colour_plane_flag ? 0 : s.chroma_format_idc;
  switch (s.chroma_format_idc) {  // Table 6-1; separate planes are coded as monochrome (§6.2)
    case 1:
      s.SubWidthC = 2;
      s.SubHeightC = 2;
      break;
    case 2:
      s.SubWidthC = 2;
      s.SubHeightC = 1;
      break;
    case 3:
      s.SubWidthC = 1;
      s.SubHeightC = 1;
      break;
    default:
      s.SubWidthC = 0;
      s.SubHeightC = 0;
      break;
  }
  if (s.ChromaArrayType == 0) {
    s.SubWidthC = 0;
    s.SubHeightC = 0;
  }
  s.BitDepthY = 8 + s.bit_depth_luma_minus8;
  s.BitDepthC = 8 + s.bit_depth_chroma_minus8;
  s.QpBdOffsetY = 6 * s.bit_depth_luma_minus8;
  s.QpBdOffsetC = 6 * s.bit_depth_chroma_minus8;
  s.MaxFrameNum = 1U << (s.log2_max_frame_num_minus4 + 4);
  s.MaxPicOrderCntLsb = 1U << (s.log2_max_pic_order_cnt_lsb_minus4 + 4);
  s.ExpectedDeltaPerPicOrderCntCycle = 0;
  for (const int32_t o : s.offset_for_ref_frame) {
    s.ExpectedDeltaPerPicOrderCntCycle += o;
  }
  s.PicWidthInMbs = s.pic_width_in_mbs_minus1 + 1;
  s.PicWidthInSamplesL = s.PicWidthInMbs * 16;
  s.PicHeightInMapUnits = s.pic_height_in_map_units_minus1 + 1;
  s.PicSizeInMapUnits = uint64_t{s.PicWidthInMbs} * s.PicHeightInMapUnits;
  s.FrameHeightInMbs = (2 - (s.frame_mbs_only_flag ? 1U : 0U)) * s.PicHeightInMapUnits;
  if (s.ChromaArrayType == 0) {  // §7.4.2.1.1 (7-19 … 7-22)
    s.CropUnitX = 1;
    s.CropUnitY = 2 - (s.frame_mbs_only_flag ? 1U : 0U);
  } else {
    s.CropUnitX = s.SubWidthC;
    s.CropUnitY = s.SubHeightC * (2 - (s.frame_mbs_only_flag ? 1U : 0U));
  }
  const auto width = int64_t{s.PicWidthInSamplesL};
  const int64_t height = int64_t{s.FrameHeightInMbs} * 16;
  s.crop_x =
      static_cast<int32_t>(std::clamp<int64_t>(int64_t{s.CropUnitX} * s.frame_crop_left_offset, INT32_MIN, INT32_MAX));
  s.crop_y =
      static_cast<int32_t>(std::clamp<int64_t>(int64_t{s.CropUnitY} * s.frame_crop_top_offset, INT32_MIN, INT32_MAX));
  s.crop_width = static_cast<int32_t>(std::clamp<int64_t>(
      width - int64_t{s.CropUnitX} * (int64_t{s.frame_crop_left_offset} + s.frame_crop_right_offset), INT32_MIN,
      INT32_MAX));
  s.crop_height = static_cast<int32_t>(std::clamp<int64_t>(
      height - int64_t{s.CropUnitY} * (int64_t{s.frame_crop_top_offset} + s.frame_crop_bottom_offset), INT32_MIN,
      INT32_MAX));
  s.MaxDpbMbs = max_dpb_mbs(s.profile_idc, s.level_idc, s.constraint_set3_flag, s.level_name);
  const uint64_t frame_mbs = uint64_t{s.PicWidthInMbs} * s.FrameHeightInMbs;
  s.MaxDpbFrames = static_cast<uint32_t>(std::min<uint64_t>(s.MaxDpbMbs / frame_mbs, 16));
  s.profile_name = profile_name(s.profile_idc, s.constraint_set1_flag, s.constraint_set3_flag, s.constraint_set4_flag,
                                s.constraint_set5_flag);

  ovt_ColorDescription& c = s.color_description;
  c = ovt_ColorDescription{};
  c.struct_size = sizeof(ovt_ColorDescription);
  c.primaries = static_cast<uint8_t>(s.vui.colour_primaries);
  c.transfer = static_cast<uint8_t>(s.vui.transfer_characteristics);
  c.matrix = static_cast<uint8_t>(s.vui.matrix_coefficients);
  c.full_range = s.vui.video_full_range_flag ? 1 : 0;
  c.chroma_siting = s.vui.chroma_loc_info_present_flag ? s.vui.chroma_siting : 0;
  c.chroma_format = static_cast<uint8_t>(s.chroma_format_idc);
  c.bit_depth = static_cast<uint8_t>(s.BitDepthY);
}

void emit_derived(SyntaxBuilder& b, const Sps& s) {
  b.derived("ChromaArrayType", s.ChromaArrayType);
  if (s.ChromaArrayType != 0) {
    b.derived("SubWidthC", s.SubWidthC);
    b.derived("SubHeightC", s.SubHeightC);
  }
  b.derived("BitDepthY", s.BitDepthY);
  b.derived("BitDepthC", s.BitDepthC);
  b.derived("QpBdOffsetY", s.QpBdOffsetY);
  b.derived("QpBdOffsetC", s.QpBdOffsetC);
  b.derived("MaxFrameNum", s.MaxFrameNum);
  if (s.pic_order_cnt_type == 0) {
    b.derived("MaxPicOrderCntLsb", s.MaxPicOrderCntLsb);
  } else if (s.pic_order_cnt_type == 1) {
    b.derived("ExpectedDeltaPerPicOrderCntCycle", s.ExpectedDeltaPerPicOrderCntCycle);
  }
  b.derived("PicWidthInMbs", s.PicWidthInMbs);
  b.derived("PicWidthInSamplesL", s.PicWidthInSamplesL);
  b.derived("PicHeightInMapUnits", s.PicHeightInMapUnits);
  b.derived("PicSizeInMapUnits", s.PicSizeInMapUnits);
  b.derived("FrameHeightInMbs", s.FrameHeightInMbs);
  b.derived("CropUnitX", s.CropUnitX);
  b.derived("CropUnitY", s.CropUnitY);
  b.derived("crop_rect", std::string_view{fmt::format("crop_rect = {}x{} at ({},{})", s.crop_width, s.crop_height,
                                                      s.crop_x, s.crop_y)});
  if (s.MaxDpbMbs != 0) {
    b.derived("MaxDpbMbs", s.MaxDpbMbs);
    b.derived("MaxDpbFrames", s.MaxDpbFrames);
  }
}

// profile_idc, constraint flags and level_idc are read by the caller; this is the block guarded by the
// high-profile profile_idc list (§7.3.2.1.1): chroma format, bit depths, transform bypass and scaling lists.
void parse_high_profile_block(BitReader& br, SyntaxBuilder& b, Sps& s) {
  s.high_profile_syntax = has_high_profile_syntax(s.profile_idc);
  if (s.high_profile_syntax) {
    s.chroma_format_idc = b.read_ue("chroma_format_idc");
    detail::require_range(br, "chroma_format_idc", s.chroma_format_idc, 0, 3);
    b.set_enum(chroma_format_name(s.chroma_format_idc));
    if (s.chroma_format_idc == 3) {
      s.separate_colour_plane_flag = b.read_flag("separate_colour_plane_flag");
    }
    s.bit_depth_luma_minus8 = b.read_ue("bit_depth_luma_minus8");
    detail::require_range(br, "bit_depth_luma_minus8", s.bit_depth_luma_minus8, 0, 6);
    s.bit_depth_chroma_minus8 = b.read_ue("bit_depth_chroma_minus8");
    detail::require_range(br, "bit_depth_chroma_minus8", s.bit_depth_chroma_minus8, 0, 6);
    s.qpprime_y_zero_transform_bypass_flag = b.read_flag("qpprime_y_zero_transform_bypass_flag");
    s.seq_scaling_matrix_present_flag = b.read_flag("seq_scaling_matrix_present_flag");
    if (s.seq_scaling_matrix_present_flag) {
      const uint32_t count = (s.chroma_format_idc != 3) ? 8 : 12;
      for (uint32_t i = 0; i < count; ++i) {
        b.begin_array_element("seq_scaling_list_present_flag", static_cast<int>(i));
        const bool present = b.read_flag("seq_scaling_list_present_flag");
        b.end_array_element();
        if (present) {
          parse_scaling_list(br, b, s.scaling_list.at(i), i < 6 ? 16 : 64, static_cast<int>(i));
        }
      }
    }
  }
}

// pic_order_cnt_type and its type-0 / type-1 elements.
void parse_pic_order_cnt_block(BitReader& br, SyntaxBuilder& b, Sps& s) {
  s.pic_order_cnt_type = b.read_ue("pic_order_cnt_type");
  detail::require_range(br, "pic_order_cnt_type", s.pic_order_cnt_type, 0, 2);
  if (s.pic_order_cnt_type == 0) {
    s.log2_max_pic_order_cnt_lsb_minus4 = b.read_ue("log2_max_pic_order_cnt_lsb_minus4");
    detail::require_range(br, "log2_max_pic_order_cnt_lsb_minus4", s.log2_max_pic_order_cnt_lsb_minus4, 0, 12);
  } else if (s.pic_order_cnt_type == 1) {
    s.delta_pic_order_always_zero_flag = b.read_flag("delta_pic_order_always_zero_flag");
    s.offset_for_non_ref_pic = b.read_se("offset_for_non_ref_pic");
    s.offset_for_top_to_bottom_field = b.read_se("offset_for_top_to_bottom_field");
    s.num_ref_frames_in_pic_order_cnt_cycle = b.read_ue("num_ref_frames_in_pic_order_cnt_cycle");
    detail::require_range(br, "num_ref_frames_in_pic_order_cnt_cycle", s.num_ref_frames_in_pic_order_cnt_cycle, 0, 255);
    s.offset_for_ref_frame.reserve(s.num_ref_frames_in_pic_order_cnt_cycle);
    for (uint32_t i = 0; i < s.num_ref_frames_in_pic_order_cnt_cycle; ++i) {
      b.begin_array_element("offset_for_ref_frame", static_cast<int>(i));
      s.offset_for_ref_frame.push_back(b.read_se("offset_for_ref_frame"));
      b.end_array_element();
    }
  }
}

}  // namespace

std::string_view profile_name(uint32_t profile_idc, bool constraint_set1_flag, bool constraint_set3_flag,
                              bool constraint_set4_flag, bool constraint_set5_flag) noexcept {
  switch (profile_idc) {  // Annex A.2
    case 66:
      return constraint_set1_flag ? "Constrained Baseline" : "Baseline";
    case 77:
      return "Main";
    case 88:
      return "Extended";
    case 100:
      if (constraint_set4_flag && constraint_set5_flag) {
        return "Constrained High";
      }
      return constraint_set4_flag ? "Progressive High" : "High";
    case 110:
      return constraint_set3_flag ? "High 10 Intra" : (constraint_set4_flag ? "Progressive High 10" : "High 10");
    case 122:
      return constraint_set3_flag ? "High 4:2:2 Intra" : "High 4:2:2";
    case 244:
      return constraint_set3_flag ? "High 4:4:4 Intra" : "High 4:4:4 Predictive";
    case 44:
      return "CAVLC 4:4:4 Intra";
    case 83:
      return constraint_set5_flag ? "Scalable Constrained Baseline" : "Scalable Baseline";
    case 86:
      return constraint_set3_flag ? "Scalable High Intra"
                                  : (constraint_set5_flag ? "Scalable Constrained High" : "Scalable High");
    case 118:
      return "Multiview High";
    case 128:
      return "Stereo High";
    case 134:
      return "MFC High";
    case 135:
      return "MFC Depth High";
    case 138:
      return "Multiview Depth High";
    case 139:
      return "Enhanced Multiview Depth High";
    default:
      return "unknown";
  }
}

uint32_t max_dpb_mbs(uint32_t profile_idc, uint32_t level_idc, bool constraint_set3_flag,
                     std::string_view& name) noexcept {
  // Level 1b: level_idc 9 (High profiles and above), or level_idc 11 with constraint_set3_flag for Baseline,
  // Main and Extended (§A.3.1).
  const bool base_profile = profile_idc == 66 || profile_idc == 77 || profile_idc == 88;
  if (level_idc == 9 || (level_idc == 11 && constraint_set3_flag && base_profile)) {
    name = "1b";
    return 396;
  }
  switch (level_idc) {  // Table A-1
    case 10:
      name = "1.0";
      return 396;
    case 11:
      name = "1.1";
      return 900;
    case 12:
      name = "1.2";
      return 2376;
    case 13:
      name = "1.3";
      return 2376;
    case 20:
      name = "2.0";
      return 2376;
    case 21:
      name = "2.1";
      return 4752;
    case 22:
      name = "2.2";
      return 8100;
    case 30:
      name = "3.0";
      return 8100;
    case 31:
      name = "3.1";
      return 18000;
    case 32:
      name = "3.2";
      return 20480;
    case 40:
      name = "4.0";
      return 32768;
    case 41:
      name = "4.1";
      return 32768;
    case 42:
      name = "4.2";
      return 34816;
    case 50:
      name = "5.0";
      return 110400;
    case 51:
      name = "5.1";
      return 184320;
    case 52:
      name = "5.2";
      return 184320;
    case 60:
      name = "6.0";
      return 696320;
    case 61:
      name = "6.1";
      return 696320;
    case 62:
      name = "6.2";
      return 696320;
    default:
      name = {};
      return 0;
  }
}

Sps parse_seq_parameter_set_data(BitReader& br, SyntaxBuilder& b) {
  Sps s;
  const auto start = br.mark();
  auto scope = b.scoped("seq_parameter_set_data", "H.264 7.3.2.1.1");
  s.profile_idc = static_cast<uint32_t>(b.read_u("profile_idc", 8));
  s.constraint_set0_flag = b.read_flag("constraint_set0_flag");
  s.constraint_set1_flag = b.read_flag("constraint_set1_flag");
  s.constraint_set2_flag = b.read_flag("constraint_set2_flag");
  s.constraint_set3_flag = b.read_flag("constraint_set3_flag");
  s.constraint_set4_flag = b.read_flag("constraint_set4_flag");
  s.constraint_set5_flag = b.read_flag("constraint_set5_flag");
  s.reserved_zero_2bits = static_cast<uint32_t>(b.read_f("reserved_zero_2bits", 2, 0));
  s.level_idc = static_cast<uint32_t>(b.read_u("level_idc", 8));
  s.profile_name = profile_name(s.profile_idc, s.constraint_set1_flag, s.constraint_set3_flag, s.constraint_set4_flag,
                                s.constraint_set5_flag);
  s.MaxDpbMbs = max_dpb_mbs(s.profile_idc, s.level_idc, s.constraint_set3_flag, s.level_name);
  if (s.level_name.empty()) {
    detail::warn_reserved(b, "level_idc", s.level_idc, "H.264 A.3");
  } else {
    b.set_enum(s.level_name);
  }
  b.derived("profile", std::string_view{fmt::format("profile = {} ({})", s.profile_name, s.profile_idc)});
  if (s.profile_name == "unknown") {
    detail::warn_reserved(b, "profile_idc", s.profile_idc, "H.264 A.2");
  }
  s.seq_parameter_set_id = b.read_ue("seq_parameter_set_id");
  detail::require_range(br, "seq_parameter_set_id", s.seq_parameter_set_id, 0, 31);
  parse_high_profile_block(br, b, s);
  s.log2_max_frame_num_minus4 = b.read_ue("log2_max_frame_num_minus4");
  detail::require_range(br, "log2_max_frame_num_minus4", s.log2_max_frame_num_minus4, 0, 12);
  parse_pic_order_cnt_block(br, b, s);
  s.max_num_ref_frames = b.read_ue("max_num_ref_frames");
  detail::require_range(br, "max_num_ref_frames", s.max_num_ref_frames, 0, 16);
  s.gaps_in_frame_num_value_allowed_flag = b.read_flag("gaps_in_frame_num_value_allowed_flag");
  s.pic_width_in_mbs_minus1 = b.read_ue("pic_width_in_mbs_minus1");
  detail::require_range(br, "pic_width_in_mbs_minus1", s.pic_width_in_mbs_minus1, 0, kMaxPicDimensionMinus1);
  s.pic_height_in_map_units_minus1 = b.read_ue("pic_height_in_map_units_minus1");
  detail::require_range(br, "pic_height_in_map_units_minus1", s.pic_height_in_map_units_minus1, 0,
                        kMaxPicDimensionMinus1);
  s.frame_mbs_only_flag = b.read_flag("frame_mbs_only_flag");
  if (!s.frame_mbs_only_flag) {
    s.mb_adaptive_frame_field_flag = b.read_flag("mb_adaptive_frame_field_flag");
  }
  s.direct_8x8_inference_flag = b.read_flag("direct_8x8_inference_flag");
  s.frame_cropping_flag = b.read_flag("frame_cropping_flag");
  if (s.frame_cropping_flag) {
    s.frame_crop_left_offset = b.read_ue("frame_crop_left_offset");
    s.frame_crop_right_offset = b.read_ue("frame_crop_right_offset");
    s.frame_crop_top_offset = b.read_ue("frame_crop_top_offset");
    s.frame_crop_bottom_offset = b.read_ue("frame_crop_bottom_offset");
  }
  s.vui_parameters_present_flag = b.read_flag("vui_parameters_present_flag");
  if (s.vui_parameters_present_flag) {
    s.vui = parse_vui_parameters(br, b);
  }
  derive(s);
  emit_derived(b, s);
  if (s.crop_width <= 0 || s.crop_height <= 0) {
    b.diag(Severity::Error, detail::kRangeCode,
           fmt::format("frame cropping exceeds the picture (crop_rect = {}x{})", s.crop_width, s.crop_height),
           "H.264 7.4.2.1.1");
  }
  s.span = br.span_since(start);
  return s;
}

Sps parse_sps(BitReader& br, SyntaxBuilder& b) {
  const auto start = br.mark();
  auto scope = b.scoped("seq_parameter_set_rbsp", "H.264 7.3.2.1");
  Sps s = parse_seq_parameter_set_data(br, b);
  detail::read_rbsp_trailing_bits(br, b);
  s.span = br.span_since(start);
  return s;
}

}  // namespace ovt::syntax::h264
