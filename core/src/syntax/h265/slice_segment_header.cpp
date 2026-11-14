// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 slice_segment_header() parser, its §7.4.7.1 derived values and the entry-point substream mapping
// (H.265 §7.3.6.1, §7.3.2.12, §7.4.7.1, §7.4.7.2; chapter 7 §7.3.5 row 10; WI-1.113).

#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/rbsp.hpp"
#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

// SPEC-ASSUMPTION(#391): chapter 7 has no H.265 code for a slice whose PPS is missing (H.264 has SYN-H264-0010)
// nor for a dependent slice segment without its independent segment; these two are the proposed codes.
constexpr std::string_view kMissingPps = "SYN-H265-0011";
constexpr std::string_view kNoIndependent = "SYN-H265-0061";

// The parameter sets the header reads, resolved once.
struct Ctx {
  const NalHeader& nal;
  const Pps& pps;
  const Sps& sps;
};

const ActivePps& activate(BitReader& br, SyntaxBuilder& b, const PpsTable& ppss, uint32_t pps_id) {
  detail::require_range(br, "slice_pic_parameter_set_id", pps_id, 0, 63);
  const ActivePps* a = ppss.entries[pps_id];
  if (a == nullptr || a->pps == nullptr || a->sps == nullptr) {
    throw ParseError{std::string{kMissingPps}, fmt::format("slice references missing PPS {}", pps_id), br.bit_pos()};
  }
  b.derived("active PPS", fmt::format("pps {} -> sps {}", pps_id, a->sps->sps_seq_parameter_set_id));
  return *a;
}

// short_term_ref_pic_set_sps_flag … short_term_ref_pic_set_idx (§7.3.6.1) and CurrRpsIdx.
void read_short_term_rps(BitReader& br, SyntaxBuilder& b, const Ctx& c, SliceSegmentHeader& sh) {
  const uint32_t num_sets = c.sps.num_short_term_ref_pic_sets;
  sh.short_term_ref_pic_set_sps_flag = b.read_flag("short_term_ref_pic_set_sps_flag");
  if (!sh.short_term_ref_pic_set_sps_flag) {
    sh.st_ref_pic_set = parse_st_ref_pic_set(br, b, num_sets, num_sets, c.sps.st_ref_pic_set, c.sps.MaxDpbSizeMinus1);
    sh.NumBitsForShortTermRPSInSlice = sh.st_ref_pic_set.span.bit_length;
    sh.CurrRpsIdx = num_sets;
    b.derived("NumBitsForShortTermRPSInSlice", sh.NumBitsForShortTermRPSInSlice);
    return;
  }
  detail::check_range(b, "num_short_term_ref_pic_sets", num_sets, 1, 64, "H.265 7.4.7.1");
  if (num_sets > 1) {
    const unsigned bits = detail::ceil_log2(num_sets);
    sh.short_term_ref_pic_set_idx = static_cast<uint32_t>(b.read_u("short_term_ref_pic_set_idx", bits));
    b.set_descriptor_text("u(v)");
    detail::require_range(br, "short_term_ref_pic_set_idx", sh.short_term_ref_pic_set_idx, 0, int64_t{num_sets} - 1);
  }
  sh.CurrRpsIdx = sh.short_term_ref_pic_set_idx;
  b.derived("CurrRpsIdx", sh.CurrRpsIdx);
}

// Long-term entries (§7.3.6.1) and PocLsbLt / UsedByCurrPicLt / DeltaPocMsbCycleLt (7-52).
void read_long_term(BitReader& br, SyntaxBuilder& b, const Ctx& c, SliceSegmentHeader& sh) {
  const uint32_t num_lt_sps = c.sps.num_long_term_ref_pics_sps;
  if (num_lt_sps > 0) {
    sh.num_long_term_sps = b.read_ue("num_long_term_sps");
    detail::require_range(br, "num_long_term_sps", sh.num_long_term_sps, 0, num_lt_sps);
  }
  sh.num_long_term_pics = b.read_ue("num_long_term_pics");
  detail::require_range(br, "num_long_term_pics", sh.num_long_term_pics, 0,
                        int64_t{kMaxLongTerm} - sh.num_long_term_sps);
  const StRefPicSet* rps = current_st_ref_pic_set(sh, c.sps);
  const int64_t st_count = rps == nullptr ? 0 : int64_t{rps->NumNegativePics} + rps->NumPositivePics;
  detail::check_range(b, "num_long_term_sps + num_long_term_pics",
                      int64_t{sh.num_long_term_sps} + sh.num_long_term_pics, 0,
                      int64_t{c.sps.MaxDpbSizeMinus1} - st_count, "H.265 7.4.7.1");
  const unsigned lsb_bits = c.sps.log2_max_pic_order_cnt_lsb_minus4 + 4U;
  const unsigned idx_bits = detail::ceil_log2(num_lt_sps);
  const int64_t max_msb_cycle = int64_t{1} << (32U - lsb_bits);
  const uint32_t total = sh.num_long_term_sps + sh.num_long_term_pics;
  for (uint32_t i = 0; i < total; ++i) {
    const auto idx = static_cast<int>(i);
    if (i < sh.num_long_term_sps) {
      if (num_lt_sps > 1) {
        sh.lt_idx_sps[i] = static_cast<uint32_t>(detail::read_u_at(b, "lt_idx_sps", idx, idx_bits, "u(v)"));
        detail::require_range(br, "lt_idx_sps", sh.lt_idx_sps[i], 0, int64_t{num_lt_sps} - 1);
      }
      sh.PocLsbLt[i] = c.sps.lt_ref_pic_poc_lsb_sps[sh.lt_idx_sps[i]];
      sh.UsedByCurrPicLt[i] = c.sps.used_by_curr_pic_lt_sps_flag[sh.lt_idx_sps[i]];
    } else {
      sh.poc_lsb_lt[i] = static_cast<uint32_t>(detail::read_u_at(b, "poc_lsb_lt", idx, lsb_bits, "u(v)"));
      sh.used_by_curr_pic_lt_flag[i] = detail::read_flag_at(b, "used_by_curr_pic_lt_flag", idx);
      sh.PocLsbLt[i] = sh.poc_lsb_lt[i];
      sh.UsedByCurrPicLt[i] = sh.used_by_curr_pic_lt_flag[i];
    }
    sh.delta_poc_msb_present_flag[i] = detail::read_flag_at(b, "delta_poc_msb_present_flag", idx);
    if (sh.delta_poc_msb_present_flag[i]) {
      sh.delta_poc_msb_cycle_lt[i] = detail::read_ue_at(b, "delta_poc_msb_cycle_lt", idx);
      detail::require_range(br, "delta_poc_msb_cycle_lt", sh.delta_poc_msb_cycle_lt[i], 0, max_msb_cycle);
    }
    sh.DeltaPocMsbCycleLt[i] = sh.delta_poc_msb_cycle_lt[i];
    if (i != 0 && i != sh.num_long_term_sps) {
      sh.DeltaPocMsbCycleLt[i] += sh.DeltaPocMsbCycleLt[i - 1];  // <= 32 * 2^28: no overflow in 64 bits
    }
  }
}

// 7-55: the pictures of the RPS used by the current picture, plus the current picture under SCC.
uint32_t num_pic_total_curr(const SliceSegmentHeader& sh, const Ctx& c) {
  uint32_t n = 0;
  if (const StRefPicSet* rps = current_st_ref_pic_set(sh, c.sps); rps != nullptr) {
    for (uint32_t i = 0; i < rps->NumNegativePics; ++i) {
      n += rps->UsedByCurrPicS0[i] ? 1U : 0U;
    }
    for (uint32_t i = 0; i < rps->NumPositivePics; ++i) {
      n += rps->UsedByCurrPicS1[i] ? 1U : 0U;
    }
  }
  for (uint32_t i = 0; i < sh.num_long_term_sps + sh.num_long_term_pics; ++i) {
    n += sh.UsedByCurrPicLt[i] ? 1U : 0U;
  }
  return n + (c.pps.scc_extension.pps_curr_pic_ref_enabled_flag ? 1U : 0U);
}

// slice_type … slice_temporal_mvp_enabled_flag: everything that depends on whether the picture is IDR.
void read_picture_part(BitReader& br, SyntaxBuilder& b, const Ctx& c, SliceSegmentHeader& sh) {
  for (unsigned i = 0; i < c.pps.num_extra_slice_header_bits; ++i) {
    sh.slice_reserved_flag[i] = detail::read_flag_at(b, "slice_reserved_flag", static_cast<int>(i));
  }
  sh.slice_type = b.read_ue("slice_type");
  detail::require_range(br, "slice_type", sh.slice_type, 0, 2);
  b.set_enum(slice_type_name(sh.slice_type));
  if (c.nal.is_irap && c.nal.nuh_layer_id == 0 && !c.pps.scc_extension.pps_curr_pic_ref_enabled_flag) {
    detail::check_range(b, "slice_type", sh.slice_type, kSliceI, kSliceI, "H.265 7.4.7.1");
  }
  if (c.pps.output_flag_present_flag) {
    sh.pic_output_flag = b.read_flag("pic_output_flag");
  }
  if (c.sps.separate_colour_plane_flag) {
    sh.colour_plane_id = static_cast<uint8_t>(b.read_u("colour_plane_id", 2));
    detail::check_range(b, "colour_plane_id", sh.colour_plane_id, 0, 2, "H.265 7.4.7.1");
  }
  if (!c.nal.is_idr) {
    sh.slice_pic_order_cnt_lsb =
        static_cast<uint32_t>(b.read_u("slice_pic_order_cnt_lsb", c.sps.log2_max_pic_order_cnt_lsb_minus4 + 4U));
    b.set_descriptor_text("u(v)");
    read_short_term_rps(br, b, c, sh);
    if (c.sps.long_term_ref_pics_present_flag) {
      read_long_term(br, b, c, sh);
    }
    if (c.sps.sps_temporal_mvp_enabled_flag) {
      sh.slice_temporal_mvp_enabled_flag = b.read_flag("slice_temporal_mvp_enabled_flag");
    }
  }
  sh.NumPicTotalCurr = num_pic_total_curr(sh, c);
  b.derived("NumPicTotalCurr", sh.NumPicTotalCurr);
  if (const StRefPicSet* rps = current_st_ref_pic_set(sh, c.sps); rps != nullptr) {
    b.derived("RPS", st_ref_pic_set_summary(*rps));
  }
}

// num_ref_idx_active_override_flag … use_integer_mv_flag (P and B slices only).
void read_inter_part(BitReader& br, SyntaxBuilder& b, const Ctx& c, SliceSegmentHeader& sh) {
  const bool is_b = sh.slice_type == kSliceB;
  sh.num_ref_idx_l0_active_minus1 = c.pps.num_ref_idx_l0_default_active_minus1;
  sh.num_ref_idx_l1_active_minus1 = is_b ? c.pps.num_ref_idx_l1_default_active_minus1 : 0;
  sh.num_ref_idx_active_override_flag = b.read_flag("num_ref_idx_active_override_flag");
  if (sh.num_ref_idx_active_override_flag) {
    sh.num_ref_idx_l0_active_minus1 = b.read_ue("num_ref_idx_l0_active_minus1");
    detail::require_range(br, "num_ref_idx_l0_active_minus1", sh.num_ref_idx_l0_active_minus1, 0, kMaxRefIdx - 1);
    if (is_b) {
      sh.num_ref_idx_l1_active_minus1 = b.read_ue("num_ref_idx_l1_active_minus1");
      detail::require_range(br, "num_ref_idx_l1_active_minus1", sh.num_ref_idx_l1_active_minus1, 0, kMaxRefIdx - 1);
    }
  }
  detail::check_range(b, "NumPicTotalCurr", sh.NumPicTotalCurr, 1, 8 + kMaxLongTerm, "H.265 7.4.7.2");
  if (c.pps.lists_modification_present_flag && sh.NumPicTotalCurr > 1) {
    sh.ref_pic_lists_modification = parse_ref_pic_lists_modification(
        br, b, sh.slice_type, sh.num_ref_idx_l0_active_minus1, sh.num_ref_idx_l1_active_minus1, sh.NumPicTotalCurr);
  }
  if (is_b) {
    sh.mvd_l1_zero_flag = b.read_flag("mvd_l1_zero_flag");
  }
  if (c.pps.cabac_init_present_flag) {
    sh.cabac_init_flag = b.read_flag("cabac_init_flag");
  }
  if (sh.slice_temporal_mvp_enabled_flag) {
    if (is_b) {
      sh.collocated_from_l0_flag = b.read_flag("collocated_from_l0_flag");
    }
    const uint32_t list_minus1 =
        sh.collocated_from_l0_flag ? sh.num_ref_idx_l0_active_minus1 : sh.num_ref_idx_l1_active_minus1;
    if (list_minus1 > 0) {
      sh.collocated_ref_idx = b.read_ue("collocated_ref_idx");
      detail::require_range(br, "collocated_ref_idx", sh.collocated_ref_idx, 0, list_minus1);
    }
  }
  if ((c.pps.weighted_pred_flag && sh.slice_type == kSliceP) || (c.pps.weighted_bipred_flag && is_b)) {
    sh.pred_weight_table = parse_pred_weight_table(br, b, c.sps, c.pps, sh.slice_type, sh.num_ref_idx_l0_active_minus1,
                                                   sh.num_ref_idx_l1_active_minus1);
  }
  sh.five_minus_max_num_merge_cand = b.read_ue("five_minus_max_num_merge_cand");
  detail::require_range(br, "five_minus_max_num_merge_cand", sh.five_minus_max_num_merge_cand, 0, 4);
  sh.MaxNumMergeCand = 5 - sh.five_minus_max_num_merge_cand;
  b.derived("MaxNumMergeCand", sh.MaxNumMergeCand);
  if (c.sps.scc_extension.motion_vector_resolution_control_idc == 2) {
    sh.use_integer_mv_flag = b.read_flag("use_integer_mv_flag");
  }
}

// slice_qp_delta … slice_loop_filter_across_slices_enabled_flag.
void read_qp_and_filters(BitReader& br, SyntaxBuilder& b, const Ctx& c, SliceSegmentHeader& sh) {
  sh.slice_qp_delta = b.read_se("slice_qp_delta");
  const int64_t qp = int64_t{26} + c.pps.init_qp_minus26 + sh.slice_qp_delta;
  detail::require_range(br, "SliceQpY", qp, -int64_t{c.sps.QpBdOffsetY}, 51);
  sh.SliceQpY = static_cast<int32_t>(qp);
  b.derived("SliceQpY", sh.SliceQpY);
  if (c.pps.pps_slice_chroma_qp_offsets_present_flag) {
    sh.slice_cb_qp_offset = b.read_se("slice_cb_qp_offset");
    detail::require_range(br, "slice_cb_qp_offset", sh.slice_cb_qp_offset, -12, 12);
    detail::check_range(b, "pps_cb_qp_offset + slice_cb_qp_offset", c.pps.pps_cb_qp_offset + sh.slice_cb_qp_offset, -12,
                        12, "H.265 7.4.7.1");
    sh.slice_cr_qp_offset = b.read_se("slice_cr_qp_offset");
    detail::require_range(br, "slice_cr_qp_offset", sh.slice_cr_qp_offset, -12, 12);
    detail::check_range(b, "pps_cr_qp_offset + slice_cr_qp_offset", c.pps.pps_cr_qp_offset + sh.slice_cr_qp_offset, -12,
                        12, "H.265 7.4.7.1");
  }
  if (c.pps.scc_extension.pps_slice_act_qp_offsets_present_flag) {
    sh.slice_act_y_qp_offset = b.read_se("slice_act_y_qp_offset");
    detail::require_range(br, "slice_act_y_qp_offset", sh.slice_act_y_qp_offset, -12, 12);
    sh.slice_act_cb_qp_offset = b.read_se("slice_act_cb_qp_offset");
    detail::require_range(br, "slice_act_cb_qp_offset", sh.slice_act_cb_qp_offset, -12, 12);
    sh.slice_act_cr_qp_offset = b.read_se("slice_act_cr_qp_offset");
    detail::require_range(br, "slice_act_cr_qp_offset", sh.slice_act_cr_qp_offset, -12, 12);
  }
  if (c.pps.range_extension.chroma_qp_offset_list_enabled_flag) {
    sh.cu_chroma_qp_offset_enabled_flag = b.read_flag("cu_chroma_qp_offset_enabled_flag");
  }
  if (c.pps.deblocking_filter_override_enabled_flag) {
    sh.deblocking_filter_override_flag = b.read_flag("deblocking_filter_override_flag");
  }
  sh.slice_deblocking_filter_disabled_flag = c.pps.pps_deblocking_filter_disabled_flag;
  sh.slice_beta_offset_div2 = c.pps.pps_beta_offset_div2;
  sh.slice_tc_offset_div2 = c.pps.pps_tc_offset_div2;
  if (sh.deblocking_filter_override_flag) {
    sh.slice_deblocking_filter_disabled_flag = b.read_flag("slice_deblocking_filter_disabled_flag");
    if (!sh.slice_deblocking_filter_disabled_flag) {
      sh.slice_beta_offset_div2 = b.read_se("slice_beta_offset_div2");
      detail::require_range(br, "slice_beta_offset_div2", sh.slice_beta_offset_div2, -6, 6);
      sh.slice_tc_offset_div2 = b.read_se("slice_tc_offset_div2");
      detail::require_range(br, "slice_tc_offset_div2", sh.slice_tc_offset_div2, -6, 6);
    }
  }
  sh.slice_loop_filter_across_slices_enabled_flag = c.pps.pps_loop_filter_across_slices_enabled_flag;
  if (c.pps.pps_loop_filter_across_slices_enabled_flag &&
      (sh.slice_sao_luma_flag || sh.slice_sao_chroma_flag || !sh.slice_deblocking_filter_disabled_flag)) {
    sh.slice_loop_filter_across_slices_enabled_flag = b.read_flag("slice_loop_filter_across_slices_enabled_flag");
  }
}

// The independent part of the header: slice_reserved_flag … slice_loop_filter_across_slices_enabled_flag.
void read_independent_part(BitReader& br, SyntaxBuilder& b, const Ctx& c, SliceSegmentHeader& sh) {
  read_picture_part(br, b, c, sh);
  if (c.sps.sample_adaptive_offset_enabled_flag) {
    sh.slice_sao_luma_flag = b.read_flag("slice_sao_luma_flag");
    if (c.sps.ChromaArrayType != 0) {
      sh.slice_sao_chroma_flag = b.read_flag("slice_sao_chroma_flag");
    }
  }
  // use_integer_mv_flag is inferred equal to motion_vector_resolution_control_idc when absent (0 or 1 here).
  sh.use_integer_mv_flag = c.sps.scc_extension.motion_vector_resolution_control_idc == 1;
  if (sh.slice_type == kSliceP || sh.slice_type == kSliceB) {
    read_inter_part(br, b, c, sh);
  }
  read_qp_and_filters(br, b, c, sh);
}

// §7.4.7.1 upper bound of num_entry_point_offsets for the PPS's tiles/WPP configuration.
uint64_t max_entry_points(const Ctx& c) {
  const uint64_t cols = c.pps.tiles_enabled_flag ? uint64_t{c.pps.num_tile_columns_minus1} + 1 : 1;
  const uint64_t rows = c.pps.tiles_enabled_flag ? uint64_t{c.pps.num_tile_rows_minus1} + 1 : 1;
  if (c.pps.tiles_enabled_flag && c.pps.entropy_coding_sync_enabled_flag) {
    return cols * c.sps.PicHeightInCtbsY - 1;
  }
  if (c.pps.tiles_enabled_flag) {
    return cols * rows - 1;
  }
  return c.sps.PicHeightInCtbsY == 0 ? 0 : uint64_t{c.sps.PicHeightInCtbsY} - 1;
}

void read_entry_points(BitReader& br, SyntaxBuilder& b, const Ctx& c, SliceSegmentHeader& sh) {
  sh.num_entry_point_offsets = b.read_ue("num_entry_point_offsets");
  // Hard bound: no configuration allows more substreams than CTBs, and every offset takes at least one bit; the
  // configuration-specific limit is a Warning (the table of §7.5 SYN-H265-0031).
  const uint64_t pic_ctbs = c.sps.PicSizeInCtbsY;
  detail::require_range(br, "num_entry_point_offsets", sh.num_entry_point_offsets, 0,
                        static_cast<int64_t>(std::min<uint64_t>(pic_ctbs == 0 ? 0 : pic_ctbs - 1, br.bits_left())));
  const uint64_t limit = max_entry_points(c);
  if (sh.num_entry_point_offsets > limit) {
    b.diag(Severity::Warning, "SYN-H265-0031",
           fmt::format("num_entry_point_offsets {} exceeds tiles/CTB rows ({})", sh.num_entry_point_offsets, limit),
           "H.265 7.4.7.1", true);
  }
  if (sh.num_entry_point_offsets == 0) {
    return;
  }
  sh.offset_len_minus1 = b.read_ue("offset_len_minus1");
  detail::require_range(br, "offset_len_minus1", sh.offset_len_minus1, 0, 31);
  const unsigned bits = sh.offset_len_minus1 + 1;
  if (uint64_t{sh.num_entry_point_offsets} * bits > br.bits_left()) {
    throw ParseError{"SYN-H265-0007",
                     fmt::format("num_entry_point_offsets out of range ({} offsets of {} bits exceed the unit)",
                                 sh.num_entry_point_offsets, bits),
                     br.bit_pos()};
  }
  sh.entry_point_offset_minus1.resize(sh.num_entry_point_offsets);
  for (uint32_t i = 0; i < sh.num_entry_point_offsets; ++i) {
    sh.entry_point_offset_minus1[i] =
        static_cast<uint32_t>(detail::read_u_at(b, "entry_point_offset_minus1", static_cast<int>(i), bits, "u(v)"));
  }
}

void read_extension(BitReader& br, SyntaxBuilder& b, SliceSegmentHeader& sh) {
  sh.slice_segment_header_extension_length = b.read_ue("slice_segment_header_extension_length");
  detail::require_range(br, "slice_segment_header_extension_length", sh.slice_segment_header_extension_length, 0, 256);
  sh.slice_segment_header_extension_data_byte.resize(sh.slice_segment_header_extension_length);
  for (uint32_t i = 0; i < sh.slice_segment_header_extension_length; ++i) {
    sh.slice_segment_header_extension_data_byte[i] =
        static_cast<uint8_t>(detail::read_u_at(b, "slice_segment_header_extension_data_byte", static_cast<int>(i), 8));
  }
}

// §7.3.2.12 byte_alignment(): one bit equal to 1, then zero bits up to the byte boundary.
void read_byte_alignment(BitReader& br, SyntaxBuilder& b) {
  auto s = b.scoped("byte_alignment", "H.265 7.3.2.12");
  (void)b.read_f("alignment_bit_equal_to_one", 1, 1);
  while (!br.byte_aligned()) {
    (void)b.read_f("alignment_bit_equal_to_zero", 1, 0);
  }
}

// A dependent slice segment takes every element of the independent part from `prev` (§7.4.7.1); the fields that
// belong to the segment itself are kept.
void copy_independent_part(SliceSegmentHeader& sh, const SliceSegmentHeader& prev) {
  const SliceSegmentHeader own = sh;
  sh = prev;
  sh.first_slice_segment_in_pic_flag = own.first_slice_segment_in_pic_flag;
  sh.no_output_of_prior_pics_flag = own.no_output_of_prior_pics_flag;
  sh.slice_pic_parameter_set_id = own.slice_pic_parameter_set_id;
  sh.dependent_slice_segment_flag = true;
  sh.slice_segment_address = own.slice_segment_address;
  sh.num_entry_point_offsets = 0;
  sh.offset_len_minus1 = 0;
  sh.entry_point_offset_minus1.clear();
  sh.slice_segment_header_extension_length = 0;
  sh.slice_segment_header_extension_data_byte.clear();
  sh.copied_from_independent = true;
  sh.tree = nullptr;
}

}  // namespace

std::string_view slice_type_name(uint32_t slice_type) noexcept {
  switch (slice_type) {
    case kSliceB:
      return "B";
    case kSliceP:
      return "P";
    case kSliceI:
      return "I";
    default:
      return "reserved";
  }
}

const StRefPicSet* current_st_ref_pic_set(const SliceSegmentHeader& sh, const Sps& sps) noexcept {
  if (!sh.short_term_ref_pic_set_sps_flag) {
    // An IDR picture signals no RPS; its default-constructed slice set is empty, which is the same thing.
    return &sh.st_ref_pic_set;
  }
  if (sh.short_term_ref_pic_set_idx < sps.st_ref_pic_set.size()) {
    return &sps.st_ref_pic_set[sh.short_term_ref_pic_set_idx];
  }
  return nullptr;
}

SliceSegmentHeader parse_slice_segment_header(BitReader& br, SyntaxBuilder& b, const NalHeader& nal,
                                              const PpsTable& ppss, const SliceSegmentHeader* prev_independent) {
  SliceSegmentHeader sh;
  const uint64_t start = br.bit_pos();
  // Opened without a Scope: slice_segment_data is the header's sibling, so the node closes before it. A ParseError
  // leaves it open and SyntaxBuilder::finish() closes it.
  b.begin_struct("slice_segment_header", "H.265 7.3.6.1");
  sh.first_slice_segment_in_pic_flag = b.read_flag("first_slice_segment_in_pic_flag");
  if (nal.nal_unit_type >= kBlaWLp && nal.nal_unit_type <= 23) {  // BLA_W_LP .. RSV_IRAP_VCL23
    sh.no_output_of_prior_pics_flag = b.read_flag("no_output_of_prior_pics_flag");
  }
  sh.slice_pic_parameter_set_id = b.read_ue("slice_pic_parameter_set_id");
  const ActivePps& active = activate(br, b, ppss, sh.slice_pic_parameter_set_id);
  const Ctx c{nal, *active.pps, *active.sps};
  if (!sh.first_slice_segment_in_pic_flag) {
    if (c.pps.dependent_slice_segments_enabled_flag) {
      sh.dependent_slice_segment_flag = b.read_flag("dependent_slice_segment_flag");
    }
    const uint32_t pic_ctbs = c.sps.PicSizeInCtbsY;
    sh.slice_segment_address = static_cast<uint32_t>(b.read_u("slice_segment_address", detail::ceil_log2(pic_ctbs)));
    b.set_descriptor_text("u(v)");
    detail::require_range(br, "slice_segment_address", sh.slice_segment_address, 0,
                          pic_ctbs == 0 ? 0 : int64_t{pic_ctbs} - 1);
  }
  if (!sh.dependent_slice_segment_flag) {
    read_independent_part(br, b, c, sh);
    sh.SliceAddrRs = sh.slice_segment_address;
  } else if (prev_independent != nullptr) {
    copy_independent_part(sh, *prev_independent);
    sh.SliceAddrRs = prev_independent->SliceAddrRs;
    b.derived("independent part",
              fmt::format("copied from the slice segment at address {} (slice_type {}, SliceQpY "
                          "{})",
                          prev_independent->slice_segment_address, slice_type_name(sh.slice_type), sh.SliceQpY));
  } else {
    b.diag(Severity::Error, kNoIndependent, "dependent slice segment without a preceding independent slice segment",
           "H.265 7.4.7.1", true);
    sh.SliceAddrRs = sh.slice_segment_address;
  }
  b.derived("SliceAddrRs", sh.SliceAddrRs);
  if (c.pps.tiles_enabled_flag || c.pps.entropy_coding_sync_enabled_flag) {
    read_entry_points(br, b, c, sh);
  }
  if (c.pps.slice_segment_header_extension_present_flag) {
    read_extension(br, b, sh);
  }
  read_byte_alignment(br, b);
  sh.span = br.span_since(start);
  b.end_struct();
  sh.slice_segment_data_rbsp_byte = br.bit_pos() / 8;
  b.opaque_rest("slice_segment_data");
  return sh;
}

std::optional<std::vector<Substream>> entry_point_substreams(const SliceSegmentHeader& sh,
                                                             std::span<const uint32_t> epb_positions,
                                                             uint64_t nal_size) {
  // The raw byte of the first slice_segment_data() byte: every EPB at or before it shifts it by one (§7.2.5).
  const uint64_t data_first = rbsp_bit_to_raw_bit(sh.slice_segment_data_rbsp_byte * 8, epb_positions) / 8;
  if (data_first > nal_size) {
    return std::nullopt;
  }
  std::vector<Substream> out;
  out.reserve(sh.entry_point_offset_minus1.size() + 1);
  uint64_t first = data_first;
  for (const uint32_t off : sh.entry_point_offset_minus1) {
    const uint64_t size = uint64_t{off} + 1;
    if (first + size >= nal_size) {  // the last substream needs at least one byte too
      return std::nullopt;
    }
    out.push_back({first, size});
    first += size;
  }
  out.push_back({first, nal_size - first});
  return out;
}

}  // namespace ovt::syntax::h265
