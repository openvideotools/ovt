// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 vui_parameters() (H.265 E.2.1): typed struct and parser (chapter 7 §7.3.5 row 7, §7.3.7; WI-1.112).
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_SYNTAX_H265_VUI_HPP
#define OVT_CXX_SYNTAX_H265_VUI_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "ovt/color.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/hrd_parameters.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// H.273 Table 2 / Table 3 / Table 4 names of a ColourPrimaries, TransferCharacteristics or MatrixCoefficients
/// code point ("BT.709", "PQ", "BT.2020-NCL", …); "reserved" for unassigned values.
[[nodiscard]] std::string_view colour_primaries_name(uint8_t code) noexcept;
[[nodiscard]] std::string_view transfer_characteristics_name(uint8_t code) noexcept;
[[nodiscard]] std::string_view matrix_coeffs_name(uint8_t code) noexcept;

/// Typed vui_parameters() (§7.3.7).
struct VuiParameters {
  bool aspect_ratio_info_present_flag = false;
  uint8_t aspect_ratio_idc = 0;
  uint16_t sar_width = 0;
  uint16_t sar_height = 0;
  bool overscan_info_present_flag = false;
  bool overscan_appropriate_flag = false;
  bool video_signal_type_present_flag = false;
  uint8_t video_format = 5;  // inferred 5 (unspecified)
  bool video_full_range_flag = false;
  bool colour_description_present_flag = false;
  uint8_t colour_primaries = 2;  // inferred 2 (unspecified)
  uint8_t transfer_characteristics = 2;
  uint8_t matrix_coeffs = 2;
  bool chroma_loc_info_present_flag = false;
  uint32_t chroma_sample_loc_type_top_field = 0;
  uint32_t chroma_sample_loc_type_bottom_field = 0;
  bool neutral_chroma_indication_flag = false;
  bool field_seq_flag = false;
  bool frame_field_info_present_flag = false;
  bool default_display_window_flag = false;
  uint32_t def_disp_win_left_offset = 0;
  uint32_t def_disp_win_right_offset = 0;
  uint32_t def_disp_win_top_offset = 0;
  uint32_t def_disp_win_bottom_offset = 0;
  bool vui_timing_info_present_flag = false;
  uint32_t vui_num_units_in_tick = 0;
  uint32_t vui_time_scale = 0;
  bool vui_poc_proportional_to_timing_flag = false;
  uint32_t vui_num_ticks_poc_diff_one_minus1 = 0;
  bool vui_hrd_parameters_present_flag = false;
  std::optional<HrdParameters> hrd_parameters;
  bool bitstream_restriction_flag = false;
  bool tiles_fixed_structure_flag = false;
  bool motion_vectors_over_pic_boundaries_flag = true;  // inferred 1
  bool restricted_ref_pic_lists_flag = false;
  uint32_t min_spatial_segmentation_idc = 0;
  uint32_t max_bytes_per_pic_denom = 2;         // inferred 2
  uint32_t max_bits_per_min_cu_denom = 1;       // inferred 1
  uint32_t log2_max_mv_length_horizontal = 15;  // inferred 15
  uint32_t log2_max_mv_length_vertical = 15;
  // derived (§7.3.5 row 7)
  double frame_rate = 0.0;  // vui_time_scale / vui_num_units_in_tick; 0 when no timing info
  /// ovt_ColorDescription candidate: primaries/transfer/matrix/full_range from the VUI, chroma_siting 3 (top-left)
  /// for the HEVC default when chroma_loc_info is absent and ChromaArrayType == 1, else the mapped
  /// chroma_sample_loc_type_top_field; chroma_format and bit_depth are filled by the SPS parser.
  ovt_ColorDescription color{};
  BitSpan span{0, 0};
};

/// Parses vui_parameters() into the node `vui_parameters`. `sps_max_sub_layers_minus1` sizes the HRD sub-layer
/// loop; `chroma_array_type` selects the default chroma siting.
VuiParameters parse_vui_parameters(BitReader& br, SyntaxBuilder& b, unsigned sps_max_sub_layers_minus1,
                                   unsigned chroma_array_type);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_VUI_HPP
