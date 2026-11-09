// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 hrd_parameters() / sub_layer_hrd_parameters() parser (H.265 E.2.2, E.2.3, E.3.2; chapter 7 §7.3.5 row 6,
// WI-1.112).

#include "ovt/cxx/syntax/h265/hrd_parameters.hpp"

#include <cstdint>
#include <string_view>

#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

void read_common_inf(SyntaxBuilder& b, HrdParameters& h) {
  h.nal_hrd_parameters_present_flag = b.read_flag("nal_hrd_parameters_present_flag");
  h.vcl_hrd_parameters_present_flag = b.read_flag("vcl_hrd_parameters_present_flag");
  if (!h.nal_hrd_parameters_present_flag && !h.vcl_hrd_parameters_present_flag) {
    return;
  }
  h.sub_pic_hrd_params_present_flag = b.read_flag("sub_pic_hrd_params_present_flag");
  if (h.sub_pic_hrd_params_present_flag) {
    h.tick_divisor_minus2 = static_cast<uint8_t>(b.read_u("tick_divisor_minus2", 8));
    h.du_cpb_removal_delay_increment_length_minus1 =
        static_cast<uint8_t>(b.read_u("du_cpb_removal_delay_increment_length_minus1", 5));
    h.sub_pic_cpb_params_in_pic_timing_sei_flag = b.read_flag("sub_pic_cpb_params_in_pic_timing_sei_flag");
    h.dpb_output_delay_du_length_minus1 = static_cast<uint8_t>(b.read_u("dpb_output_delay_du_length_minus1", 5));
  }
  h.bit_rate_scale = static_cast<uint8_t>(b.read_u("bit_rate_scale", 4));
  h.cpb_size_scale = static_cast<uint8_t>(b.read_u("cpb_size_scale", 4));
  if (h.sub_pic_hrd_params_present_flag) {
    h.cpb_size_du_scale = static_cast<uint8_t>(b.read_u("cpb_size_du_scale", 4));
  }
  h.initial_cpb_removal_delay_length_minus1 =
      static_cast<uint8_t>(b.read_u("initial_cpb_removal_delay_length_minus1", 5));
  h.au_cpb_removal_delay_length_minus1 = static_cast<uint8_t>(b.read_u("au_cpb_removal_delay_length_minus1", 5));
  h.dpb_output_delay_length_minus1 = static_cast<uint8_t>(b.read_u("dpb_output_delay_length_minus1", 5));
}

// E.2.3 sub_layer_hrd_parameters(subLayerId); the node is named after the caller (nal_hrd / vcl_hrd).
SubLayerHrdParameters read_sub_layer_hrd(SyntaxBuilder& b, const HrdParameters& h, std::string_view node_name,
                                         int sub_layer_id, uint32_t cpb_cnt_minus1) {
  SubLayerHrdParameters out;
  b.begin_array_element(node_name, sub_layer_id, "H.265 E.2.3");
  out.cpb.resize(static_cast<size_t>(cpb_cnt_minus1) + 1);
  for (uint32_t i = 0; i <= cpb_cnt_minus1; ++i) {
    CpbParameters& c = out.cpb[i];
    const int idx = static_cast<int>(i);
    c.bit_rate_value_minus1 = detail::read_ue_at(b, "bit_rate_value_minus1", idx);
    c.cpb_size_value_minus1 = detail::read_ue_at(b, "cpb_size_value_minus1", idx);
    if (h.sub_pic_hrd_params_present_flag) {
      c.cpb_size_du_value_minus1 = detail::read_ue_at(b, "cpb_size_du_value_minus1", idx);
      c.bit_rate_du_value_minus1 = detail::read_ue_at(b, "bit_rate_du_value_minus1", idx);
    }
    c.cbr_flag = detail::read_flag_at(b, "cbr_flag", idx);
    c.BitRate = (uint64_t{c.bit_rate_value_minus1} + 1) << (6U + h.bit_rate_scale);
    c.CpbSize = (uint64_t{c.cpb_size_value_minus1} + 1) << (4U + h.cpb_size_scale);
    b.derived("BitRate", std::string_view{fmt::format("BitRate[{}] = {}", i, c.BitRate)});
    b.derived("CpbSize", std::string_view{fmt::format("CpbSize[{}] = {}", i, c.CpbSize)});
  }
  b.end_array_element();
  return out;
}

void read_sub_layer(BitReader& br, SyntaxBuilder& b, HrdParameters& h, unsigned i) {
  SubLayerHrdInfo& sl = h.sub_layer[i];
  const int idx = static_cast<int>(i);
  sl.fixed_pic_rate_general_flag = detail::read_flag_at(b, "fixed_pic_rate_general_flag", idx);
  sl.fixed_pic_rate_within_cvs_flag = true;  // inferred when fixed_pic_rate_general_flag == 1 (E.3.2)
  if (!sl.fixed_pic_rate_general_flag) {
    sl.fixed_pic_rate_within_cvs_flag = detail::read_flag_at(b, "fixed_pic_rate_within_cvs_flag", idx);
  }
  if (sl.fixed_pic_rate_within_cvs_flag) {
    sl.elemental_duration_in_tc_minus1 = detail::read_ue_at(b, "elemental_duration_in_tc_minus1", idx);
    detail::check_range(b, "elemental_duration_in_tc_minus1", sl.elemental_duration_in_tc_minus1, 0, 2047,
                        "H.265 E.3.2");
  } else {
    sl.low_delay_hrd_flag = detail::read_flag_at(b, "low_delay_hrd_flag", idx);
  }
  if (!sl.low_delay_hrd_flag) {
    sl.cpb_cnt_minus1 = detail::read_ue_at(b, "cpb_cnt_minus1", idx);
    detail::require_range(br, "cpb_cnt_minus1", sl.cpb_cnt_minus1, 0, 31);
  }
  if (h.nal_hrd_parameters_present_flag) {
    sl.nal = read_sub_layer_hrd(b, h, "nal_sub_layer_hrd_parameters", idx, sl.cpb_cnt_minus1);
  }
  if (h.vcl_hrd_parameters_present_flag) {
    sl.vcl = read_sub_layer_hrd(b, h, "vcl_sub_layer_hrd_parameters", idx, sl.cpb_cnt_minus1);
  }
}

// §7.4.3.1: with cprms_present_flag[i] == 0 the common information equals that of the (i-1)-th hrd_parameters().
void inherit_common_inf(HrdParameters& h, const HrdParameters& from) {
  h.nal_hrd_parameters_present_flag = from.nal_hrd_parameters_present_flag;
  h.vcl_hrd_parameters_present_flag = from.vcl_hrd_parameters_present_flag;
  h.sub_pic_hrd_params_present_flag = from.sub_pic_hrd_params_present_flag;
  h.tick_divisor_minus2 = from.tick_divisor_minus2;
  h.du_cpb_removal_delay_increment_length_minus1 = from.du_cpb_removal_delay_increment_length_minus1;
  h.sub_pic_cpb_params_in_pic_timing_sei_flag = from.sub_pic_cpb_params_in_pic_timing_sei_flag;
  h.dpb_output_delay_du_length_minus1 = from.dpb_output_delay_du_length_minus1;
  h.bit_rate_scale = from.bit_rate_scale;
  h.cpb_size_scale = from.cpb_size_scale;
  h.cpb_size_du_scale = from.cpb_size_du_scale;
  h.initial_cpb_removal_delay_length_minus1 = from.initial_cpb_removal_delay_length_minus1;
  h.au_cpb_removal_delay_length_minus1 = from.au_cpb_removal_delay_length_minus1;
  h.dpb_output_delay_length_minus1 = from.dpb_output_delay_length_minus1;
}

}  // namespace

HrdParameters parse_hrd_parameters(BitReader& br, SyntaxBuilder& b, bool common_inf_present_flag,
                                   unsigned max_num_sub_layers_minus1, const HrdParameters* inherited) {
  detail::require_range(br, "maxNumSubLayersMinus1", max_num_sub_layers_minus1, 0, 6);
  HrdParameters h;
  h.common_inf_present_flag = common_inf_present_flag;
  h.max_num_sub_layers_minus1 = static_cast<uint8_t>(max_num_sub_layers_minus1);
  const uint64_t start = br.bit_pos();
  auto s = b.scoped("hrd_parameters", "H.265 E.2.2");
  if (common_inf_present_flag) {
    read_common_inf(b, h);
  } else if (inherited != nullptr) {
    inherit_common_inf(h, *inherited);
    b.derived("common_inf", std::string_view{"inherited from the previous hrd_parameters() (7.4.3.1)"});
  }
  for (unsigned i = 0; i <= max_num_sub_layers_minus1; ++i) {
    read_sub_layer(br, b, h, i);
  }
  h.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return h;
}

}  // namespace ovt::syntax::h265
