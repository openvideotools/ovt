// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 pic_parameter_set_rbsp() (§7.3.2.2) with FMO slice group map types 0–6, the more_rbsp_data() tail and
// the provisional parse before the SPS: chapter 7 §7.3.4 row 6 and §7.3.7, WI-1.108. C++ helper header (D22):
// semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H264_PPS_HPP
#define OVT_CXX_SYNTAX_H264_PPS_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264 {

/// The SPS entries a PPS parse may consult (§7.3.7 `const SpsTable&`): the latest received SPS per id, or
/// nullptr. ActiveSets (§7.3.8, WI-1.111) fills one from its sps[32] table.
struct SpsTable {
  std::array<const Sps*, 32> entries{};
  [[nodiscard]] const Sps* find(uint32_t seq_parameter_set_id) const noexcept {
    return seq_parameter_set_id < entries.size() ? entries[seq_parameter_set_id] : nullptr;
  }
};

struct Pps {
  uint32_t pic_parameter_set_id = 0;
  uint32_t seq_parameter_set_id = 0;
  bool entropy_coding_mode_flag = false;
  bool bottom_field_pic_order_in_frame_present_flag = false;
  uint32_t num_slice_groups_minus1 = 0;
  uint32_t slice_group_map_type = 0;
  std::vector<uint32_t> run_length_minus1;  // map type 0, one per slice group
  std::vector<uint32_t> top_left;           // map type 2, num_slice_groups_minus1 entries
  std::vector<uint32_t> bottom_right;
  bool slice_group_change_direction_flag = false;  // map types 3..5
  uint32_t slice_group_change_rate_minus1 = 0;
  uint32_t pic_size_in_map_units_minus1 = 0;  // map type 6
  std::vector<uint8_t> slice_group_id;        // pic_size_in_map_units_minus1 + 1 entries
  uint32_t num_ref_idx_l0_default_active_minus1 = 0;
  uint32_t num_ref_idx_l1_default_active_minus1 = 0;
  bool weighted_pred_flag = false;
  uint32_t weighted_bipred_idc = 0;
  int32_t pic_init_qp_minus26 = 0;
  int32_t pic_init_qs_minus26 = 0;
  int32_t chroma_qp_index_offset = 0;
  bool deblocking_filter_control_present_flag = false;
  bool constrained_intra_pred_flag = false;
  bool redundant_pic_cnt_present_flag = false;
  bool has_tail = false;  // more_rbsp_data() was true: the three elements below were read
  bool transform_8x8_mode_flag = false;
  bool pic_scaling_matrix_present_flag = false;
  std::array<ScalingList, 12> scaling_list{};  // pic_scaling_list_present_flag[i] is scaling_list[i].present
  uint32_t num_scaling_lists = 0;              // 6 + ((chroma_format_idc != 3) ? 2 : 6) * transform_8x8_mode_flag
  int32_t second_chroma_qp_index_offset = 0;   // defaults to chroma_qp_index_offset when absent
  // derived
  uint32_t SliceGroupChangeRate = 1;  // slice_group_change_rate_minus1 + 1 (map types 3..5)
  uint32_t slice_group_id_bits = 0;   // Ceil(Log2(num_slice_groups_minus1 + 1)), the u(v) width of slice_group_id
  bool provisional = false;           // parsed without its SPS (chroma_format_idc assumed 1); re-parse later (§7.3.8)
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 6. Node `pic_parameter_set_rbsp`. The reader stands after the NAL header. `sps_table.find(id)` supplies
/// the SPS for the scaling-list count and the QP range checks; when it is nullptr the PPS is parsed
/// provisionally (Diagnostic Warning SYN-H264-0020 when the assumed count mattered).
Pps parse_pps(BitReader& br, SyntaxBuilder& b, const SpsTable& sps_table);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_PPS_HPP
