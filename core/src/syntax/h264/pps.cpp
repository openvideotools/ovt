// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 pic_parameter_set_rbsp() with FMO map types 0–6, the more_rbsp_data() tail and the provisional parse
// before the SPS (chapter 7 §7.3.4 row 6, WI-1.108). Element order follows H.264 (08/2021) §7.3.2.2 line by line.

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

constexpr uint32_t kMaxFrameSizeMbs = 139264;  // Table A-1 MaxFS of level 6.2: bound for pic_size_in_map_units_minus1

std::string_view slice_group_map_type_name(uint32_t t) noexcept {
  switch (t) {  // Table 7-4 / §7.4.2.2
    case 0:
      return "interleaved";
    case 1:
      return "dispersed";
    case 2:
      return "foreground with left-over";
    case 3:
      return "box-out";
    case 4:
      return "raster scan";
    case 5:
      return "wipe";
    case 6:
      return "explicit";
    default:
      return {};
  }
}

std::string_view weighted_bipred_name(uint32_t idc) noexcept {
  switch (idc) {
    case 0:
      return "default";
    case 1:
      return "explicit";
    case 2:
      return "implicit";
    default:
      return {};
  }
}

void parse_slice_groups(BitReader& br, SyntaxBuilder& b, Pps& p, const Sps* sps) {
  p.slice_group_map_type = b.read_ue("slice_group_map_type");
  detail::require_range(br, "slice_group_map_type", p.slice_group_map_type, 0, 6);
  b.set_enum(slice_group_map_type_name(p.slice_group_map_type));
  const uint32_t groups = p.num_slice_groups_minus1 + 1;
  if (p.slice_group_map_type == 0) {
    p.run_length_minus1.reserve(groups);
    for (uint32_t i = 0; i < groups; ++i) {
      b.begin_array_element("run_length_minus1", static_cast<int>(i));
      p.run_length_minus1.push_back(b.read_ue("run_length_minus1"));
      b.end_array_element();
    }
  } else if (p.slice_group_map_type == 2) {
    p.top_left.reserve(p.num_slice_groups_minus1);
    p.bottom_right.reserve(p.num_slice_groups_minus1);
    for (uint32_t i = 0; i < p.num_slice_groups_minus1; ++i) {
      b.begin_array_element("slice_group", static_cast<int>(i));
      p.top_left.push_back(b.read_ue("top_left"));
      p.bottom_right.push_back(b.read_ue("bottom_right"));
      b.end_array_element();
    }
  } else if (p.slice_group_map_type >= 3 && p.slice_group_map_type <= 5) {
    p.slice_group_change_direction_flag = b.read_flag("slice_group_change_direction_flag");
    p.slice_group_change_rate_minus1 = b.read_ue("slice_group_change_rate_minus1");
    p.SliceGroupChangeRate = p.slice_group_change_rate_minus1 + 1;
    b.derived("SliceGroupChangeRate", p.SliceGroupChangeRate);
  } else if (p.slice_group_map_type == 6) {
    p.pic_size_in_map_units_minus1 = b.read_ue("pic_size_in_map_units_minus1");
    detail::require_range(br, "pic_size_in_map_units_minus1", p.pic_size_in_map_units_minus1, 0, kMaxFrameSizeMbs - 1);
    if (sps != nullptr && p.pic_size_in_map_units_minus1 + 1 != sps->PicSizeInMapUnits) {
      b.diag(Severity::Error, detail::kRangeCode,
             fmt::format("pic_size_in_map_units_minus1 + 1 = {} differs from PicSizeInMapUnits = {}",
                         p.pic_size_in_map_units_minus1 + 1, sps->PicSizeInMapUnits),
             "H.264 7.4.2.2", true);
    }
    p.slice_group_id_bits = detail::ceil_log2(groups);
    b.derived("slice_group_id_bits", p.slice_group_id_bits);
    const uint32_t n = p.pic_size_in_map_units_minus1 + 1;
    p.slice_group_id.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      b.begin_array_element("slice_group_id", static_cast<int>(i));
      const auto id = static_cast<uint8_t>(b.read_u("slice_group_id", p.slice_group_id_bits));
      b.set_descriptor_text("u(v)");
      b.end_array_element();
      p.slice_group_id[i] = id;
    }
  }
}

// pic_init_qp_minus26 … chroma_qp_index_offset with their semantic ranges (§7.4.2.2); violations are Warnings.
void parse_qp_offsets(SyntaxBuilder& b, Pps& p, uint32_t qp_bd_offset_y) {
  p.pic_init_qp_minus26 = b.read_se("pic_init_qp_minus26");
  if (p.pic_init_qp_minus26 < -(26 + static_cast<int32_t>(qp_bd_offset_y)) || p.pic_init_qp_minus26 > 25) {
    detail::warn_reserved(b, "pic_init_qp_minus26", p.pic_init_qp_minus26, "H.264 7.4.2.2");
  }
  p.pic_init_qs_minus26 = b.read_se("pic_init_qs_minus26");
  if (p.pic_init_qs_minus26 < -26 || p.pic_init_qs_minus26 > 25) {
    detail::warn_reserved(b, "pic_init_qs_minus26", p.pic_init_qs_minus26, "H.264 7.4.2.2");
  }
  p.chroma_qp_index_offset = b.read_se("chroma_qp_index_offset");
  if (p.chroma_qp_index_offset < -12 || p.chroma_qp_index_offset > 12) {
    detail::warn_reserved(b, "chroma_qp_index_offset", p.chroma_qp_index_offset, "H.264 7.4.2.2");
  }
}

// pic_scaling_matrix_present_flag == 1: 6 + ((chroma_format_idc != 3) ? 2 : 6) * transform_8x8_mode_flag lists.
void parse_pic_scaling_matrix(BitReader& br, SyntaxBuilder& b, Pps& p, uint32_t chroma_format_idc) {
  p.num_scaling_lists = 6 + ((chroma_format_idc != 3) ? 2U : 6U) * (p.transform_8x8_mode_flag ? 1U : 0U);
  if (p.provisional && p.transform_8x8_mode_flag) {
    // SPEC-ASSUMPTION(#320): raised only when the assumed count was used.
    b.diag(Severity::Warning, "SYN-H264-0020", "PPS parsed before its SPS; scaling list count assumed",
           "H.264 7.3.2.2");
  }
  for (uint32_t i = 0; i < p.num_scaling_lists; ++i) {
    b.begin_array_element("pic_scaling_list_present_flag", static_cast<int>(i));
    const bool present = b.read_flag("pic_scaling_list_present_flag");
    b.end_array_element();
    if (present) {
      parse_scaling_list(br, b, p.scaling_list.at(i), i < 6 ? 16 : 64, static_cast<int>(i));
    }
  }
}

// The more_rbsp_data() tail: transform_8x8_mode_flag, pic_scaling_matrix_present_flag + lists,
// second_chroma_qp_index_offset; inferred values when absent.
void parse_tail(BitReader& br, SyntaxBuilder& b, Pps& p, uint32_t chroma_format_idc) {
  if (br.more_rbsp_data()) {
    p.has_tail = true;
    p.transform_8x8_mode_flag = b.read_flag("transform_8x8_mode_flag");
    p.pic_scaling_matrix_present_flag = b.read_flag("pic_scaling_matrix_present_flag");
    if (p.pic_scaling_matrix_present_flag) {
      parse_pic_scaling_matrix(br, b, p, chroma_format_idc);
    }
    p.second_chroma_qp_index_offset = b.read_se("second_chroma_qp_index_offset");
    if (p.second_chroma_qp_index_offset < -12 || p.second_chroma_qp_index_offset > 12) {
      detail::warn_reserved(b, "second_chroma_qp_index_offset", p.second_chroma_qp_index_offset, "H.264 7.4.2.2");
    }
  } else {
    b.derived("transform_8x8_mode_flag", std::string_view{"transform_8x8_mode_flag = 0 (inferred)"});
    b.derived("second_chroma_qp_index_offset",
              std::string_view{fmt::format("second_chroma_qp_index_offset = {} (inferred)", p.chroma_qp_index_offset)});
  }
}

}  // namespace

Pps parse_pps(BitReader& br, SyntaxBuilder& b, const SpsTable& sps_table) {
  Pps p;
  const auto start = br.mark();
  auto scope = b.scoped("pic_parameter_set_rbsp", "H.264 7.3.2.2");
  p.pic_parameter_set_id = b.read_ue("pic_parameter_set_id");
  detail::require_range(br, "pic_parameter_set_id", p.pic_parameter_set_id, 0, 255);
  p.seq_parameter_set_id = b.read_ue("seq_parameter_set_id");
  detail::require_range(br, "seq_parameter_set_id", p.seq_parameter_set_id, 0, 31);
  const Sps* sps = sps_table.find(p.seq_parameter_set_id);
  p.provisional = sps == nullptr;
  // SPEC-ASSUMPTION(#320): the SPS is unknown → chroma_format_idc 1 for the scaling-list count (row 6 pitfall).
  const uint32_t chroma_format_idc = (sps != nullptr) ? sps->chroma_format_idc : 1;
  const uint32_t qp_bd_offset_y = (sps != nullptr) ? sps->QpBdOffsetY : 0;
  p.entropy_coding_mode_flag = b.read_flag("entropy_coding_mode_flag");
  b.set_enum(p.entropy_coding_mode_flag ? "CABAC" : "CAVLC");
  p.bottom_field_pic_order_in_frame_present_flag = b.read_flag("bottom_field_pic_order_in_frame_present_flag");
  p.num_slice_groups_minus1 = b.read_ue("num_slice_groups_minus1");
  detail::require_range(br, "num_slice_groups_minus1", p.num_slice_groups_minus1, 0, 7);
  if (p.num_slice_groups_minus1 > 0) {
    parse_slice_groups(br, b, p, sps);
  }
  p.num_ref_idx_l0_default_active_minus1 = b.read_ue("num_ref_idx_l0_default_active_minus1");
  detail::require_range(br, "num_ref_idx_l0_default_active_minus1", p.num_ref_idx_l0_default_active_minus1, 0, 31);
  p.num_ref_idx_l1_default_active_minus1 = b.read_ue("num_ref_idx_l1_default_active_minus1");
  detail::require_range(br, "num_ref_idx_l1_default_active_minus1", p.num_ref_idx_l1_default_active_minus1, 0, 31);
  p.weighted_pred_flag = b.read_flag("weighted_pred_flag");
  p.weighted_bipred_idc = static_cast<uint32_t>(b.read_u("weighted_bipred_idc", 2));
  if (p.weighted_bipred_idc <= 2) {
    b.set_enum(weighted_bipred_name(p.weighted_bipred_idc));
  } else {
    detail::warn_reserved(b, "weighted_bipred_idc", p.weighted_bipred_idc, "H.264 7.4.2.2");
  }
  parse_qp_offsets(b, p, qp_bd_offset_y);
  p.deblocking_filter_control_present_flag = b.read_flag("deblocking_filter_control_present_flag");
  p.constrained_intra_pred_flag = b.read_flag("constrained_intra_pred_flag");
  p.redundant_pic_cnt_present_flag = b.read_flag("redundant_pic_cnt_present_flag");
  p.second_chroma_qp_index_offset = p.chroma_qp_index_offset;
  parse_tail(br, b, p, chroma_format_idc);
  if (p.provisional) {
    b.derived("provisional", std::string_view{"provisional = 1 (SPS not yet received)"});
  }
  detail::read_rbsp_trailing_bits(br, b);
  p.span = br.span_since(start);
  return p;
}

}  // namespace ovt::syntax::h264
