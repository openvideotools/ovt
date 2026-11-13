// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 video_parameter_set_rbsp() (H.265 §7.3.2.1): typed struct and parser (chapter 7 §7.3.5 row 2, §7.3.7;
// WI-1.112). C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_SYNTAX_H265_VPS_HPP
#define OVT_CXX_SYNTAX_H265_VPS_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/hrd_parameters.hpp"
#include "ovt/cxx/syntax/h265/profile_tier_level.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// Per sub-layer DPB parameters of a VPS or SPS (§7.4.3.1 / §7.4.3.2.1); entries below the first signalled
/// sub-layer are inferred equal to the highest one.
struct SubLayerOrderingInfo {
  uint32_t max_dec_pic_buffering_minus1 = 0;
  uint32_t max_num_reorder_pics = 0;
  uint32_t max_latency_increase_plus1 = 0;
};

/// One hrd_parameters() entry of the VPS.
struct VpsHrd {
  uint32_t hrd_layer_set_idx = 0;
  bool cprms_present_flag = true;  // inferred 1 for i == 0
  HrdParameters hrd_parameters;
};

/// Typed video_parameter_set_rbsp() (§7.3.7).
struct Vps {
  uint8_t vps_video_parameter_set_id = 0;
  bool vps_base_layer_internal_flag = false;
  bool vps_base_layer_available_flag = false;
  uint8_t vps_max_layers_minus1 = 0;
  uint8_t vps_max_sub_layers_minus1 = 0;
  bool vps_temporal_id_nesting_flag = false;
  uint16_t vps_reserved_0xffff_16bits = 0;
  ProfileTierLevel profile_tier_level;
  bool vps_sub_layer_ordering_info_present_flag = false;
  std::array<SubLayerOrderingInfo, 7> sub_layer_ordering_info;  // vps_max_dec_pic_buffering_minus1[i] etc.
  uint8_t vps_max_layer_id = 0;
  uint32_t vps_num_layer_sets_minus1 = 0;
  std::vector<uint64_t>
      layer_id_included_flag;  // [i]: bit j = layer_id_included_flag[i][j], i = 0..num_layer_sets_minus1
  bool vps_timing_info_present_flag = false;
  uint32_t vps_num_units_in_tick = 0;
  uint32_t vps_time_scale = 0;
  bool vps_poc_proportional_to_timing_flag = false;
  uint32_t vps_num_ticks_poc_diff_one_minus1 = 0;
  uint32_t vps_num_hrd_parameters = 0;
  std::vector<VpsHrd> hrd;
  bool vps_extension_flag = false;
  bool vps_extension_opaque = false;  // vps_extension() kept as opaque bytes (phase 1)
  // derived
  double frame_rate = 0.0;  // vps_time_scale / vps_num_units_in_tick hint; 0 when absent
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;  // set by the owning Unit
};

/// Parses video_parameter_set_rbsp() after the NAL header into the node `video_parameter_set_rbsp`.
Vps parse_vps(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_VPS_HPP
