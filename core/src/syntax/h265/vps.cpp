// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 video_parameter_set_rbsp() parser (H.265 §7.3.2.1, §7.4.3.1; chapter 7 §7.3.5 row 2, WI-1.112).

#include "ovt/cxx/syntax/h265/vps.hpp"

#include <cstdint>
#include <string_view>

#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

void read_sub_layer_ordering_info(BitReader& br, SyntaxBuilder& b, Vps& v) {
  const unsigned first = v.vps_sub_layer_ordering_info_present_flag ? 0U : v.vps_max_sub_layers_minus1;
  for (unsigned i = first; i <= v.vps_max_sub_layers_minus1; ++i) {
    SubLayerOrderingInfo& o = v.sub_layer_ordering_info[i];
    const int idx = static_cast<int>(i);
    o.max_dec_pic_buffering_minus1 = detail::read_ue_at(b, "vps_max_dec_pic_buffering_minus1", idx);
    detail::require_range(br, "vps_max_dec_pic_buffering_minus1", o.max_dec_pic_buffering_minus1, 0, 15);  // DPB size
    o.max_num_reorder_pics = detail::read_ue_at(b, "vps_max_num_reorder_pics", idx);
    detail::check_range(b, "vps_max_num_reorder_pics", o.max_num_reorder_pics, 0, o.max_dec_pic_buffering_minus1,
                        "H.265 7.4.3.1");
    o.max_latency_increase_plus1 = detail::read_ue_at(b, "vps_max_latency_increase_plus1", idx);
  }
  for (unsigned i = 0; i < first; ++i) {  // inferred equal to the highest sub-layer (§7.4.3.1)
    v.sub_layer_ordering_info[i] = v.sub_layer_ordering_info[v.vps_max_sub_layers_minus1];
  }
}

void read_layer_sets(BitReader& br, SyntaxBuilder& b, Vps& v) {
  v.vps_max_layer_id = static_cast<uint8_t>(b.read_u("vps_max_layer_id", 6));
  detail::check_range(b, "vps_max_layer_id", v.vps_max_layer_id, 0, 62, "H.265 7.4.3.1");
  v.vps_num_layer_sets_minus1 = b.read_ue("vps_num_layer_sets_minus1");
  detail::require_range(br, "vps_num_layer_sets_minus1", v.vps_num_layer_sets_minus1, 0, 1023);
  v.layer_id_included_flag.assign(static_cast<size_t>(v.vps_num_layer_sets_minus1) + 1, 0);
  v.layer_id_included_flag[0] = 1;  // layer set 0 contains layer 0 only (§7.4.3.1)
  for (uint32_t i = 1; i <= v.vps_num_layer_sets_minus1; ++i) {
    b.begin_array_element("layer_set", static_cast<int>(i));
    uint64_t mask = 0;
    for (unsigned j = 0; j <= v.vps_max_layer_id; ++j) {
      if (detail::read_flag_at(b, "layer_id_included_flag", static_cast<int>(j))) {
        mask |= uint64_t{1} << j;
      }
    }
    v.layer_id_included_flag[i] = mask;
    b.end_array_element();
  }
}

void read_timing_and_hrd(BitReader& br, SyntaxBuilder& b, Vps& v) {
  v.vps_num_units_in_tick = static_cast<uint32_t>(b.read_u("vps_num_units_in_tick", 32));
  v.vps_time_scale = static_cast<uint32_t>(b.read_u("vps_time_scale", 32));
  v.vps_poc_proportional_to_timing_flag = b.read_flag("vps_poc_proportional_to_timing_flag");
  if (v.vps_poc_proportional_to_timing_flag) {
    v.vps_num_ticks_poc_diff_one_minus1 = b.read_ue("vps_num_ticks_poc_diff_one_minus1");
  }
  if (v.vps_num_units_in_tick > 0) {
    v.frame_rate = static_cast<double>(v.vps_time_scale) / static_cast<double>(v.vps_num_units_in_tick);
    b.derived("frame_rate", std::string_view{fmt::format("frame_rate = {:g}", v.frame_rate)});
  }
  v.vps_num_hrd_parameters = b.read_ue("vps_num_hrd_parameters");
  detail::require_range(br, "vps_num_hrd_parameters", v.vps_num_hrd_parameters, 0,
                        int64_t{v.vps_num_layer_sets_minus1} + 1);
  v.hrd.resize(v.vps_num_hrd_parameters);
  for (uint32_t i = 0; i < v.vps_num_hrd_parameters; ++i) {
    VpsHrd& h = v.hrd[i];
    b.begin_array_element("vps_hrd", static_cast<int>(i));
    h.hrd_layer_set_idx = b.read_ue("hrd_layer_set_idx");
    detail::check_range(b, "hrd_layer_set_idx", h.hrd_layer_set_idx, v.vps_base_layer_internal_flag ? 0 : 1,
                        v.vps_num_layer_sets_minus1, "H.265 7.4.3.1");
    if (i > 0) {
      h.cprms_present_flag = b.read_flag("cprms_present_flag");
    }
    // 7.4.3.1: cprms_present_flag[i] == 0 derives the common information from the (i-1)-th hrd_parameters().
    const HrdParameters* previous = i > 0 ? &v.hrd[i - 1].hrd_parameters : nullptr;
    h.hrd_parameters = parse_hrd_parameters(br, b, h.cprms_present_flag, v.vps_max_sub_layers_minus1, previous);
    b.end_array_element();
  }
}

}  // namespace

Vps parse_vps(BitReader& br, SyntaxBuilder& b) {
  Vps v;
  const uint64_t start = br.bit_pos();
  auto s = b.scoped("video_parameter_set_rbsp", "H.265 7.3.2.1");
  v.vps_video_parameter_set_id = static_cast<uint8_t>(b.read_u("vps_video_parameter_set_id", 4));
  v.vps_base_layer_internal_flag = b.read_flag("vps_base_layer_internal_flag");
  v.vps_base_layer_available_flag = b.read_flag("vps_base_layer_available_flag");
  v.vps_max_layers_minus1 = static_cast<uint8_t>(b.read_u("vps_max_layers_minus1", 6));
  detail::check_range(b, "vps_max_layers_minus1", v.vps_max_layers_minus1, 0, 62, "H.265 7.4.3.1");
  v.vps_max_sub_layers_minus1 = static_cast<uint8_t>(b.read_u("vps_max_sub_layers_minus1", 3));
  detail::require_range(br, "vps_max_sub_layers_minus1", v.vps_max_sub_layers_minus1, 0, 6);
  v.vps_temporal_id_nesting_flag = b.read_flag("vps_temporal_id_nesting_flag");
  if (v.vps_max_sub_layers_minus1 == 0 && !v.vps_temporal_id_nesting_flag) {
    b.diag(Severity::Error, "SYN-H265-0007",
           "vps_temporal_id_nesting_flag out of range (vps_temporal_id_nesting_flag = 0, allowed 1 when "
           "vps_max_sub_layers_minus1 == 0)",
           "H.265 7.4.3.1", true);
  }
  v.vps_reserved_0xffff_16bits = static_cast<uint16_t>(b.read_f("vps_reserved_0xffff_16bits", 16, 0xFFFF));
  v.profile_tier_level = parse_profile_tier_level(br, b, true, v.vps_max_sub_layers_minus1);
  v.vps_sub_layer_ordering_info_present_flag = b.read_flag("vps_sub_layer_ordering_info_present_flag");
  read_sub_layer_ordering_info(br, b, v);
  read_layer_sets(br, b, v);
  v.vps_timing_info_present_flag = b.read_flag("vps_timing_info_present_flag");
  if (v.vps_timing_info_present_flag) {
    read_timing_and_hrd(br, b, v);
  }
  v.vps_extension_flag = b.read_flag("vps_extension_flag");
  if (v.vps_extension_flag) {
    // Phase 1: vps_extension() (F.7.3.2.1.1) and the trailing bits stay opaque (§7.3.5 row 2).
    v.vps_extension_opaque = true;
    b.opaque_rest("vps_extension");
  } else {
    detail::read_rbsp_trailing_bits(br, b);
  }
  v.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return v;
}

}  // namespace ovt::syntax::h265
