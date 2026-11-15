// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 slice_header() (§7.3.3) with PPS → SPS activation and the derived variables of §7.4.3, and the
// slice_layer_without_partitioning_rbsp() / MVC slice_layer_extension_rbsp() wrappers: chapter 7 §7.3.4 row 24,
// WI-1.109. Element order follows H.264 (08/2021) §7.3.3 line by line.

#include "ovt/cxx/syntax/h264/slice_header.hpp"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

constexpr std::string_view kMissingParameterSet = "SYN-H264-0010";

// nal_unit_type 20 / 21 whose header carries the MVC extension: H.7.3.3.4 reads slice_header() with the subset SPS.
bool is_mvc_slice(const NalHeader& nal) noexcept {
  return (nal.nal_unit_type == 20 || nal.nal_unit_type == 21) && nal.has_mvc_extension;
}

// IdrPicFlag (§7.4.1 and H.7.4.1.1): nal_unit_type 5, or an MVC slice whose non_idr_flag is 0.
bool idr_pic_flag(const NalHeader& nal) noexcept {
  if (nal.nal_unit_type == 5) {
    return true;
  }
  return is_mvc_slice(nal) && !nal.mvc.non_idr_flag;
}

// Ceil(Log2(n)) for 64-bit n >= 1.
uint32_t ceil_log2_u64(uint64_t n) noexcept {
  uint32_t bits = 0;
  while (bits < 64 && (uint64_t{1} << bits) < n) {
    ++bits;
  }
  return bits;
}

struct Active {
  const Pps& pps;
  const Sps& sps;
};

// §7.3.8: pic_parameter_set_id selects the PPS, its seq_parameter_set_id the SPS (the subset SPS for NAL 20 / 21).
Active activate(BitReader& br, SyntaxBuilder& b, const NalHeader& nal, const ParameterSets& sets, SliceHeader& sh) {
  detail::require_range(br, "pic_parameter_set_id", sh.pic_parameter_set_id, 0, 255);
  const Pps* pps = sets.pps.find(sh.pic_parameter_set_id);
  if (pps == nullptr) {
    throw ParseError{std::string{kMissingParameterSet},
                     fmt::format("slice references missing PPS {}", sh.pic_parameter_set_id), br.bit_pos()};
  }
  sh.uses_subset_sps = nal.nal_unit_type == 20 || nal.nal_unit_type == 21;
  const Sps* sps = (sh.uses_subset_sps ? sets.subset_sps : sets.sps).find(pps->seq_parameter_set_id);
  if (sps == nullptr) {
    // SPEC-ASSUMPTION(#529): a PPS whose SPS is missing is reported with the missing-PPS code SYN-H264-0010.
    throw ParseError{std::string{kMissingParameterSet},
                     fmt::format("slice references PPS {} whose {} {} is missing", sh.pic_parameter_set_id,
                                 sh.uses_subset_sps ? "subset SPS" : "SPS", pps->seq_parameter_set_id),
                     br.bit_pos()};
  }
  sh.seq_parameter_set_id = pps->seq_parameter_set_id;
  b.derived("active parameter sets",
            std::string_view{fmt::format("active parameter sets = PPS {} -> {} {}", sh.pic_parameter_set_id,
                                         sh.uses_subset_sps ? "subset SPS" : "SPS", sh.seq_parameter_set_id)});
  if (sets.on_activate) {
    sets.on_activate(nal, *pps, *sps);
  }
  return Active{*pps, *sps};
}

// frame_num … delta_pic_order_cnt[1] (the picture-identification part of §7.3.3).
void read_picture_id(BitReader& br, SyntaxBuilder& b, const Active& a, SliceHeader& sh) {
  const Sps& sps = a.sps;
  if (sps.separate_colour_plane_flag) {
    sh.colour_plane_id = static_cast<uint32_t>(b.read_u("colour_plane_id", 2));
    detail::check_range(b, "colour_plane_id", sh.colour_plane_id, 0, 2, "H.264 7.4.3");
  }
  sh.frame_num = static_cast<uint32_t>(b.read_u("frame_num", sps.log2_max_frame_num_minus4 + 4));
  b.set_descriptor_text("u(v)");
  if (sh.IdrPicFlag) {
    detail::check_range(b, "frame_num", sh.frame_num, 0, 0, "H.264 7.4.3");
  }
  if (!sps.frame_mbs_only_flag) {
    sh.field_pic_flag = b.read_flag("field_pic_flag");
    if (sh.field_pic_flag) {
      sh.bottom_field_flag = b.read_flag("bottom_field_flag");
    }
  }
  if (sh.IdrPicFlag) {
    sh.idr_pic_id = b.read_ue("idr_pic_id");
    detail::require_range(br, "idr_pic_id", sh.idr_pic_id, 0, 65535);
  }
  const bool bottom_present = a.pps.bottom_field_pic_order_in_frame_present_flag && !sh.field_pic_flag;
  if (sps.pic_order_cnt_type == 0) {
    sh.pic_order_cnt_lsb =
        static_cast<uint32_t>(b.read_u("pic_order_cnt_lsb", sps.log2_max_pic_order_cnt_lsb_minus4 + 4));
    b.set_descriptor_text("u(v)");
    if (bottom_present) {
      sh.delta_pic_order_cnt_bottom = b.read_se("delta_pic_order_cnt_bottom");
    }
  }
  if (sps.pic_order_cnt_type == 1 && !sps.delta_pic_order_always_zero_flag) {
    sh.delta_pic_order_cnt[0] = detail::read_se_at(b, "delta_pic_order_cnt", 0);
    if (bottom_present) {
      sh.delta_pic_order_cnt[1] = detail::read_se_at(b, "delta_pic_order_cnt", 1);
    }
  }
  if (a.pps.redundant_pic_cnt_present_flag) {
    sh.redundant_pic_cnt = b.read_ue("redundant_pic_cnt");
    detail::require_range(br, "redundant_pic_cnt", sh.redundant_pic_cnt, 0, 127);
  }
}

// direct_spatial_mv_pred_flag … num_ref_idx_l1_active_minus1, with the PPS defaults (§7.4.3).
void read_ref_idx_counts(BitReader& br, SyntaxBuilder& b, const Active& a, SliceHeader& sh) {
  const uint32_t t = sh.slice_type_mod5;
  sh.num_ref_idx_l0_active_minus1 = a.pps.num_ref_idx_l0_default_active_minus1;
  sh.num_ref_idx_l1_active_minus1 = a.pps.num_ref_idx_l1_default_active_minus1;
  if (t == kSliceB) {
    sh.direct_spatial_mv_pred_flag = b.read_flag("direct_spatial_mv_pred_flag");
  }
  if (t != kSliceP && t != kSliceSP && t != kSliceB) {
    return;
  }
  sh.num_ref_idx_active_override_flag = b.read_flag("num_ref_idx_active_override_flag");
  if (sh.num_ref_idx_active_override_flag) {
    sh.num_ref_idx_l0_active_minus1 = b.read_ue("num_ref_idx_l0_active_minus1");
    detail::require_range(br, "num_ref_idx_l0_active_minus1", sh.num_ref_idx_l0_active_minus1, 0, kMaxRefIdx - 1);
    if (t == kSliceB) {
      sh.num_ref_idx_l1_active_minus1 = b.read_ue("num_ref_idx_l1_active_minus1");
      detail::require_range(br, "num_ref_idx_l1_active_minus1", sh.num_ref_idx_l1_active_minus1, 0, kMaxRefIdx - 1);
    }
  }
  b.derived("num_ref_idx_l0_active_minus1", sh.num_ref_idx_l0_active_minus1);
  if (t == kSliceB) {
    b.derived("num_ref_idx_l1_active_minus1", sh.num_ref_idx_l1_active_minus1);
  }
  // §7.4.3: a frame uses at most 16 references per list (0..15); 0..31 is the field range.
  if (!sh.field_pic_flag) {
    if (sh.num_ref_idx_l0_active_minus1 > 15) {
      b.diag(Severity::Error, detail::kRangeCode,
             fmt::format("num_ref_idx_l0_active_minus1 = {} exceeds 15 in a frame", sh.num_ref_idx_l0_active_minus1),
             "H.264 7.4.3");
    }
    if (t == kSliceB && sh.num_ref_idx_l1_active_minus1 > 15) {
      b.diag(Severity::Error, detail::kRangeCode,
             fmt::format("num_ref_idx_l1_active_minus1 = {} exceeds 15 in a frame", sh.num_ref_idx_l1_active_minus1),
             "H.264 7.4.3");
    }
  }
}

// cabac_init_idc … slice_group_change_cycle (the part after dec_ref_pic_marking()).
void read_qp_and_filter(BitReader& br, SyntaxBuilder& b, const Active& a, SliceHeader& sh) {
  const Pps& pps = a.pps;
  const uint32_t t = sh.slice_type_mod5;
  if (pps.entropy_coding_mode_flag && t != kSliceI && t != kSliceSI) {
    sh.cabac_init_idc = b.read_ue("cabac_init_idc");
    detail::require_range(br, "cabac_init_idc", sh.cabac_init_idc, 0, 2);
  }
  sh.slice_qp_delta = b.read_se("slice_qp_delta");
  const int64_t qp = 26 + int64_t{pps.pic_init_qp_minus26} + sh.slice_qp_delta;
  detail::require_range(br, "SliceQPY", qp, -static_cast<int64_t>(a.sps.QpBdOffsetY), 51);
  sh.SliceQPY = static_cast<int32_t>(qp);
  b.derived("SliceQPY", sh.SliceQPY);
  if (t == kSliceSP || t == kSliceSI) {
    if (t == kSliceSP) {
      sh.sp_for_switch_flag = b.read_flag("sp_for_switch_flag");
    }
    sh.slice_qs_delta = b.read_se("slice_qs_delta");
    const int64_t qs = 26 + int64_t{pps.pic_init_qs_minus26} + sh.slice_qs_delta;
    detail::require_range(br, "QSY", qs, 0, 51);
    sh.QSY = static_cast<int32_t>(qs);
    b.derived("QSY", sh.QSY);
  }
  if (pps.deblocking_filter_control_present_flag) {
    sh.disable_deblocking_filter_idc = b.read_ue("disable_deblocking_filter_idc");
    detail::require_range(br, "disable_deblocking_filter_idc", sh.disable_deblocking_filter_idc, 0, 2);
    if (sh.disable_deblocking_filter_idc != 1) {
      sh.slice_alpha_c0_offset_div2 = b.read_se("slice_alpha_c0_offset_div2");
      detail::require_range(br, "slice_alpha_c0_offset_div2", sh.slice_alpha_c0_offset_div2, -6, 6);
      sh.slice_beta_offset_div2 = b.read_se("slice_beta_offset_div2");
      detail::require_range(br, "slice_beta_offset_div2", sh.slice_beta_offset_div2, -6, 6);
    }
  }
  sh.FilterOffsetA = sh.slice_alpha_c0_offset_div2 * 2;  // << 1 (7-32)
  sh.FilterOffsetB = sh.slice_beta_offset_div2 * 2;
  if (pps.deblocking_filter_control_present_flag && sh.disable_deblocking_filter_idc != 1) {
    b.derived("FilterOffsetA", sh.FilterOffsetA);
    b.derived("FilterOffsetB", sh.FilterOffsetB);
  }
  if (pps.num_slice_groups_minus1 > 0 && pps.slice_group_map_type >= 3 && pps.slice_group_map_type <= 5) {
    // Ceil(Log2(PicSizeInMapUnits ÷ SliceGroupChangeRate + 1)) with exact division (§7.4.3): 2^v − 1 must reach
    // Ceil(PicSizeInMapUnits ÷ SliceGroupChangeRate), the largest allowed slice_group_change_cycle.
    const uint64_t rate = uint64_t{pps.slice_group_change_rate_minus1} + 1;
    const uint64_t max_cycle = (a.sps.PicSizeInMapUnits + rate - 1) / rate;
    sh.slice_group_change_cycle_bits = ceil_log2_u64(max_cycle + 1);
    sh.slice_group_change_cycle =
        static_cast<uint32_t>(b.read_u("slice_group_change_cycle", sh.slice_group_change_cycle_bits));
    b.set_descriptor_text("u(v)");
    detail::check_range(b, "slice_group_change_cycle", sh.slice_group_change_cycle, 0, static_cast<int64_t>(max_cycle),
                        "H.264 7.4.3");
    b.derived("slice_group_change_cycle_bits", sh.slice_group_change_cycle_bits);
  }
}

constexpr std::array<std::string_view, 4> kStructureNames{"Frame", "TopField", "BottomField", "Mbaff"};

// MbaffFrameFlag … CurrPicNum and PictureStructure (§7.4.3), emitted after field_pic_flag is known.
void derive_picture(SyntaxBuilder& b, const Sps& sps, SliceHeader& sh) {
  sh.MbaffFrameFlag = sps.mb_adaptive_frame_field_flag && !sh.field_pic_flag;
  sh.PicHeightInMbs = sps.FrameHeightInMbs / (sh.field_pic_flag ? 2U : 1U);
  sh.PicSizeInMbs = uint64_t{sps.PicWidthInMbs} * sh.PicHeightInMbs;
  sh.MaxPicNum = sh.field_pic_flag ? 2 * sps.MaxFrameNum : sps.MaxFrameNum;
  sh.CurrPicNum = sh.field_pic_flag ? 2 * sh.frame_num + 1 : sh.frame_num;
  if (sh.field_pic_flag) {
    sh.structure = sh.bottom_field_flag ? PictureStructure::BottomField : PictureStructure::TopField;
  } else {
    sh.structure = sh.MbaffFrameFlag ? PictureStructure::Mbaff : PictureStructure::Frame;
  }
  b.derived("MbaffFrameFlag", sh.MbaffFrameFlag ? 1 : 0);
  b.derived("PicHeightInMbs", sh.PicHeightInMbs);
  b.derived("PicSizeInMbs", sh.PicSizeInMbs);
  b.derived("MaxPicNum", sh.MaxPicNum);
  b.derived("CurrPicNum", sh.CurrPicNum);
  b.derived("PictureStructure", kStructureNames.at(static_cast<size_t>(sh.structure)));
  // §7.4.3: first_mb_in_slice * (1 + MbaffFrameFlag) < PicSizeInMbs.
  const uint64_t first = uint64_t{sh.first_mb_in_slice} * (sh.MbaffFrameFlag ? 2U : 1U);
  if (first >= sh.PicSizeInMbs) {
    b.diag(Severity::Error, detail::kRangeCode,
           fmt::format("first_mb_in_slice = {} outside the picture (PicSizeInMbs = {}, MbaffFrameFlag = {})",
                       sh.first_mb_in_slice, sh.PicSizeInMbs, sh.MbaffFrameFlag ? 1 : 0),
           "H.264 7.4.3");
  }
}

}  // namespace

std::string_view slice_type_name(uint32_t slice_type) noexcept {
  constexpr std::array<std::string_view, 5> kNames{"P", "B", "I", "SP", "SI"};  // Table 7-6
  return slice_type > 9 ? std::string_view{"reserved"} : kNames[slice_type % 5];
}

SliceHeader parse_slice_header(BitReader& br, SyntaxBuilder& b, const NalHeader& nal, const ParameterSets& sets) {
  const uint32_t nut = nal.nal_unit_type;
  if (nut != 1 && nut != 2 && nut != 5 && !is_mvc_slice(nal)) {
    // SPEC-ASSUMPTION(#529): SVC (G.7.3.3.4) and 3D-AVC slice headers are WI-4.111's.
    throw ParseError{std::string{detail::kRangeCode},
                     fmt::format("slice_header() is not defined for nal_unit_type {} with this extension header", nut),
                     br.bit_pos()};
  }
  SliceHeader sh;
  sh.IdrPicFlag = idr_pic_flag(nal);
  const auto start = br.mark();
  auto scope = b.scoped("slice_header", "H.264 7.3.3");
  sh.first_mb_in_slice = b.read_ue("first_mb_in_slice");
  sh.slice_type = b.read_ue("slice_type");
  detail::require_range(br, "slice_type", sh.slice_type, 0, 9);
  sh.slice_type_mod5 = static_cast<SliceType>(sh.slice_type % 5);
  sh.slice_type_name = slice_type_name(sh.slice_type);
  b.set_enum(sh.slice_type_name);
  if (sh.IdrPicFlag && sh.slice_type_mod5 != kSliceI && sh.slice_type_mod5 != kSliceSI) {
    // §7.4.3: an IDR picture contains only I or SI slices.
    b.diag(Severity::Error, detail::kRangeCode,
           fmt::format("slice_type = {} ({}) in an IDR picture", sh.slice_type, sh.slice_type_name), "H.264 7.4.3",
           true);
  }
  sh.pic_parameter_set_id = b.read_ue("pic_parameter_set_id");
  const Active a = activate(br, b, nal, sets, sh);
  read_picture_id(br, b, a, sh);
  derive_picture(b, a.sps, sh);
  read_ref_idx_counts(br, b, a, sh);
  sh.ref_pic_list_modification = parse_ref_pic_list_modification(br, b, sh.slice_type, sh.num_ref_idx_l0_active_minus1,
                                                                 sh.num_ref_idx_l1_active_minus1, is_mvc_slice(nal));
  const uint32_t t = sh.slice_type_mod5;
  sh.has_pred_weight_table =
      (a.pps.weighted_pred_flag && (t == kSliceP || t == kSliceSP)) || (a.pps.weighted_bipred_idc == 1 && t == kSliceB);
  if (sh.has_pred_weight_table) {
    sh.pred_weight_table = parse_pred_weight_table(br, b, sh.slice_type, a.sps.ChromaArrayType,
                                                   sh.num_ref_idx_l0_active_minus1, sh.num_ref_idx_l1_active_minus1);
  }
  sh.has_dec_ref_pic_marking = nal.nal_ref_idc != 0;
  if (sh.has_dec_ref_pic_marking) {
    sh.dec_ref_pic_marking = parse_dec_ref_pic_marking(br, b, sh.IdrPicFlag, a.sps.max_num_ref_frames);
  }
  read_qp_and_filter(br, b, a, sh);
  sh.header_bits = br.bit_pos();
  sh.span = br.span_since(start);
  return sh;
}

SliceHeader parse_slice_layer(BitReader& br, SyntaxBuilder& b, const NalHeader& nal, const ParameterSets& sets) {
  const bool ext = nal.nal_unit_type == 20 || nal.nal_unit_type == 21;
  auto scope = b.scoped(ext ? "slice_layer_extension_rbsp" : "slice_layer_without_partitioning_rbsp",
                        ext ? "H.264 7.3.2.13" : "H.264 7.3.2.8");
  SliceHeader sh = parse_slice_header(br, b, nal, sets);
  b.opaque_rest("slice_data");
  return sh;
}

}  // namespace ovt::syntax::h264
