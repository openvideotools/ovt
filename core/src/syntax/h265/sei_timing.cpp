// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 buffering_period() (D.2.2), pic_timing() (D.2.3) and recovery_point() (D.2.8): chapter 7 §7.3.5 row 14,
// WI-1.114. The HRD field lengths come from the hrd_parameters() of the SPS VUI.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ovt/cxx/syntax/h265/sei.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "syntax/h265/h265_internal.h"
#include "syntax/h265/sei_internal.h"

namespace ovt::syntax::h265::detail {

namespace {

// Table D-2.
constexpr std::array<std::string_view, 13> kPicStructNames{
    "frame",
    "top field",
    "bottom field",
    "top field, bottom field, in that order",
    "bottom field, top field, in that order",
    "top field, bottom field, top field repeated, in that order",
    "bottom field, top field, bottom field repeated, in that order",
    "frame doubling",
    "frame tripling",
    "top field paired with previous bottom field in output order",
    "bottom field paired with previous top field in output order",
    "top field paired with next bottom field in output order",
    "bottom field paired with next top field in output order",
};

// The CPB entries of one HRD (NAL or VCL). CpbCnt is cpb_cnt_minus1[0] + 1, as HM (SEIReader) and FFmpeg read it.
// SPEC-ASSUMPTION(#570): D.2.2 names CpbCnt without saying which sub-layer's cpb_cnt_minus1 it is.
std::vector<InitialCpbRemoval> read_cpb_entries(SeiPayloadReader& r, const HrdParameters& hrd, bool vcl,
                                                bool alt_present) {
  const unsigned bits = hrd.initial_cpb_removal_delay_length_minus1 + 1U;
  const uint32_t count = hrd.sub_layer[0].cpb_cnt_minus1 + 1;
  const std::string_view delay = vcl ? "vcl_initial_cpb_removal_delay" : "nal_initial_cpb_removal_delay";
  const std::string_view offset = vcl ? "vcl_initial_cpb_removal_offset" : "nal_initial_cpb_removal_offset";
  const std::string_view alt_delay = vcl ? "vcl_initial_alt_cpb_removal_delay" : "nal_initial_alt_cpb_removal_delay";
  const std::string_view alt_offset = vcl ? "vcl_initial_alt_cpb_removal_offset" : "nal_initial_alt_cpb_removal_offset";
  std::vector<InitialCpbRemoval> out(count);
  for (uint32_t i = 0; i < count; ++i) {
    const auto idx = static_cast<int>(i);
    InitialCpbRemoval& e = out[i];
    e.initial_cpb_removal_delay = sei_uv_at(r, delay, idx, bits);
    e.initial_cpb_removal_offset = sei_uv_at(r, offset, idx, bits);
    if (alt_present) {
      e.initial_alt_cpb_removal_delay = sei_uv_at(r, alt_delay, idx, bits);
      e.initial_alt_cpb_removal_offset = sei_uv_at(r, alt_offset, idx, bits);
    }
  }
  return out;
}

}  // namespace

BufferingPeriod parse_buffering_period(SeiPayloadReader& r, const Sps& sps) {
  BufferingPeriod bp;
  bp.bp_seq_parameter_set_id = r.ue("bp_seq_parameter_set_id");
  require_range(r.reader(), "bp_seq_parameter_set_id", bp.bp_seq_parameter_set_id, 0, 15);
  const HrdParameters* hrd = sps_hrd(sps);
  const bool sub_pic = hrd != nullptr && hrd->sub_pic_hrd_params_present_flag;
  if (!sub_pic) {
    bp.irap_cpb_params_present_flag = r.flag("irap_cpb_params_present_flag");
  }
  // Without an HRD every length takes its inferred value (E.3.2: 23 + 1 bits).
  const unsigned au_bits = (hrd != nullptr ? hrd->au_cpb_removal_delay_length_minus1 : 23U) + 1U;
  const unsigned dpb_bits = (hrd != nullptr ? hrd->dpb_output_delay_length_minus1 : 23U) + 1U;
  if (bp.irap_cpb_params_present_flag) {
    bp.cpb_delay_offset = sei_uv(r, "cpb_delay_offset", au_bits);
    bp.dpb_delay_offset = sei_uv(r, "dpb_delay_offset", dpb_bits);
  }
  bp.concatenation_flag = r.flag("concatenation_flag");
  bp.au_cpb_removal_delay_delta_minus1 = sei_uv(r, "au_cpb_removal_delay_delta_minus1", au_bits);
  bp.alt_present = sub_pic || bp.irap_cpb_params_present_flag;
  if (hrd != nullptr && hrd->nal_hrd_parameters_present_flag) {
    bp.nal = read_cpb_entries(r, *hrd, false, bp.alt_present);
  }
  if (hrd != nullptr && hrd->vcl_hrd_parameters_present_flag) {
    bp.vcl = read_cpb_entries(r, *hrd, true, bp.alt_present);
  }
  if (payload_extension_present(r)) {
    bp.use_alt_cpb_params_flag = r.flag("use_alt_cpb_params_flag");
  }
  return bp;
}

PicTiming parse_pic_timing(SeiPayloadReader& r, const Sps& sps) {
  PicTiming pt;
  SyntaxBuilder& b = r.builder();
  pt.frame_field_info_present = sps.vui_parameters_present_flag && sps.vui && sps.vui->frame_field_info_present_flag;
  if (pt.frame_field_info_present) {
    pt.pic_struct = static_cast<uint32_t>(r.u("pic_struct", 4));
    pt.pic_struct_name = pt.pic_struct < kPicStructNames.size() ? kPicStructNames[pt.pic_struct] : "reserved";
    b.set_enum(pt.pic_struct_name);
    pt.source_scan_type = static_cast<uint32_t>(r.u("source_scan_type", 2));
    pt.duplicate_flag = r.flag("duplicate_flag");
  }
  const HrdParameters* hrd = sps_hrd(sps);
  pt.cpb_dpb_delays_present =
      hrd != nullptr && (hrd->nal_hrd_parameters_present_flag || hrd->vcl_hrd_parameters_present_flag);
  if (!pt.cpb_dpb_delays_present) {
    return pt;
  }
  pt.au_cpb_removal_delay_minus1 =
      sei_uv(r, "au_cpb_removal_delay_minus1", hrd->au_cpb_removal_delay_length_minus1 + 1U);
  pt.pic_dpb_output_delay = sei_uv(r, "pic_dpb_output_delay", hrd->dpb_output_delay_length_minus1 + 1U);
  if (hrd->sub_pic_hrd_params_present_flag) {
    pt.pic_dpb_output_du_delay = sei_uv(r, "pic_dpb_output_du_delay", hrd->dpb_output_delay_du_length_minus1 + 1U);
  }
  pt.du_info_present = hrd->sub_pic_hrd_params_present_flag && hrd->sub_pic_cpb_params_in_pic_timing_sei_flag;
  if (!pt.du_info_present) {
    return pt;
  }
  const unsigned du_bits = hrd->du_cpb_removal_delay_increment_length_minus1 + 1U;
  pt.num_decoding_units_minus1 = r.ue("num_decoding_units_minus1");
  // D.3.3: 0..PicSizeInCtbsY - 1. Each unit reads at least one bit, so the payload bound also limits the loop.
  require_range(r.reader(), "num_decoding_units_minus1", pt.num_decoding_units_minus1, 0,
                sps.PicSizeInCtbsY > 0 ? int64_t{sps.PicSizeInCtbsY} - 1 : 0);
  pt.du_common_cpb_removal_delay_flag = r.flag("du_common_cpb_removal_delay_flag");
  if (pt.du_common_cpb_removal_delay_flag) {
    pt.du_common_cpb_removal_delay_increment_minus1 =
        sei_uv(r, "du_common_cpb_removal_delay_increment_minus1", du_bits);
  }
  for (uint32_t i = 0; i <= pt.num_decoding_units_minus1; ++i) {
    const auto idx = static_cast<int>(i);
    pt.num_nalus_in_du_minus1.push_back(sei_ue_at(r, "num_nalus_in_du_minus1", idx));
    if (!pt.du_common_cpb_removal_delay_flag && i < pt.num_decoding_units_minus1) {
      pt.du_cpb_removal_delay_increment_minus1.push_back(
          sei_uv_at(r, "du_cpb_removal_delay_increment_minus1", idx, du_bits));
    }
  }
  return pt;
}

RecoveryPoint parse_recovery_point(SeiPayloadReader& r, const Sps* sps) {
  RecoveryPoint p;
  p.recovery_poc_cnt = r.se("recovery_poc_cnt");
  if (sps != nullptr) {  // D.3.8: -MaxPicOrderCntLsb / 2 .. MaxPicOrderCntLsb / 2 - 1
    const int64_t half = int64_t{sps->MaxPicOrderCntLsb} / 2;
    (void)check_range(r.builder(), "recovery_poc_cnt", p.recovery_poc_cnt, -half, half - 1, "H.265 D.3.8");
  }
  p.exact_match_flag = r.flag("exact_match_flag");
  p.broken_link_flag = r.flag("broken_link_flag");
  return p;
}

}  // namespace ovt::syntax::h265::detail
