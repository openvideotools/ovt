// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 hrd_parameters() and sub_layer_hrd_parameters() (H.265 E.2.2, E.2.3): typed structs and parser
// (chapter 7 §7.3.5 row 6, §7.3.7; WI-1.112). C++ helper header (D22): semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H265_HRD_PARAMETERS_HPP
#define OVT_CXX_SYNTAX_H265_HRD_PARAMETERS_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// One CPB entry of sub_layer_hrd_parameters() (E.2.3) with its derived E.3.2 values.
struct CpbParameters {
  uint32_t bit_rate_value_minus1 = 0;
  uint32_t cpb_size_value_minus1 = 0;
  uint32_t cpb_size_du_value_minus1 = 0;  // sub_pic_hrd_params_present_flag
  uint32_t bit_rate_du_value_minus1 = 0;  // sub_pic_hrd_params_present_flag
  bool cbr_flag = false;
  // derived (E-xx): BitRate = (bit_rate_value_minus1 + 1) << (6 + bit_rate_scale), CpbSize likewise with 4 +
  // cpb_size_scale
  uint64_t BitRate = 0;
  uint64_t CpbSize = 0;
};

/// sub_layer_hrd_parameters(subLayerId): CpbCnt = cpb_cnt_minus1[subLayerId] + 1 entries.
struct SubLayerHrdParameters {
  std::vector<CpbParameters> cpb;
};

/// Per sub-layer fields of hrd_parameters() (E.2.2).
struct SubLayerHrdInfo {
  bool fixed_pic_rate_general_flag = false;
  bool fixed_pic_rate_within_cvs_flag = false;  // inferred 1 when fixed_pic_rate_general_flag
  uint32_t elemental_duration_in_tc_minus1 = 0;
  bool low_delay_hrd_flag = false;
  uint32_t cpb_cnt_minus1 = 0;
  SubLayerHrdParameters nal;  // nal_hrd_parameters_present_flag
  SubLayerHrdParameters vcl;  // vcl_hrd_parameters_present_flag
};

/// Typed hrd_parameters(commonInfPresentFlag, maxNumSubLayersMinus1) (§7.3.7).
struct HrdParameters {
  bool common_inf_present_flag = false;
  uint8_t max_num_sub_layers_minus1 = 0;
  bool nal_hrd_parameters_present_flag = false;
  bool vcl_hrd_parameters_present_flag = false;
  bool sub_pic_hrd_params_present_flag = false;
  uint8_t tick_divisor_minus2 = 0;
  uint8_t du_cpb_removal_delay_increment_length_minus1 = 0;
  bool sub_pic_cpb_params_in_pic_timing_sei_flag = false;
  uint8_t dpb_output_delay_du_length_minus1 = 0;
  uint8_t bit_rate_scale = 0;
  uint8_t cpb_size_scale = 0;
  uint8_t cpb_size_du_scale = 0;
  uint8_t initial_cpb_removal_delay_length_minus1 = 23;  // inferred 23 when absent (E.3.2)
  uint8_t au_cpb_removal_delay_length_minus1 = 23;
  uint8_t dpb_output_delay_length_minus1 = 23;
  std::array<SubLayerHrdInfo, 7> sub_layer;  // valid for i <= max_num_sub_layers_minus1
  BitSpan span{0, 0};
};

/// Parses hrd_parameters(commonInfPresentFlag, maxNumSubLayersMinus1) into the node `hrd_parameters`.
/// When commonInfPresentFlag is 0 the common information (nal/vcl present flags, sub-picture parameters, scales
/// and lengths) is not in the bitstream; §7.4.3.1 derives it from the previous hrd_parameters() of the VPS, which
/// the caller passes as `inherited` (vps_hrd[i-1]; nullptr keeps the E.3.2 inferred defaults, i.e. no NAL/VCL
/// sub-layer parameters, as for cprms_present_flag[0] which is always 1). cpb_cnt_minus1 > 31 or
/// maxNumSubLayersMinus1 > 6 throw ParseError SYN-H265-0007.
HrdParameters parse_hrd_parameters(BitReader& br, SyntaxBuilder& b, bool common_inf_present_flag,
                                   unsigned max_num_sub_layers_minus1, const HrdParameters* inherited = nullptr);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_HRD_PARAMETERS_HPP
