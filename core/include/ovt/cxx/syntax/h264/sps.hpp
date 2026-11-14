// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 seq_parameter_set_data() (§7.3.2.1.1), scaling_list() (§7.3.2.1.1.1), vui_parameters() (E.1.1) and
// hrd_parameters() (E.1.2) with their derived variables: chapter 7 §7.3.4 rows 2–5 and §7.3.7, WI-1.108.
// C++ helper header (D22): semi-public, no ABI promise.
//
// Every struct holds the syntax elements under their spec names, the derived variables under the spec's
// variable names, the BitSpan of the structure and (set by the owning Unit, not by the parser) the tree.

#ifndef OVT_CXX_SYNTAX_H264_SPS_HPP
#define OVT_CXX_SYNTAX_H264_SPS_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ovt/color.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {
class SyntaxNodeBuffer;
}

namespace ovt::syntax::h264 {

/// scaling_list(scalingList, sizeOfScalingList, useDefaultScalingMatrixFlag) (§7.3.2.1.1.1), row 3.
struct ScalingList {
  bool present = false;  // seq_/pic_scaling_list_present_flag[i]
  uint32_t size = 16;    // 16 (lists 0..5) or 64 (lists 6..11)
  std::array<int32_t, 64> delta_scale{};
  std::array<uint8_t, 64> list{};  // scalingList[j] in zig-zag order; entries >= size are 0
  bool UseDefaultScalingMatrixFlag = false;
  BitSpan span{0, 0};
};

/// hrd_parameters() (E.1.2), row 5.
struct HrdParameters {
  uint32_t cpb_cnt_minus1 = 0;
  uint32_t bit_rate_scale = 0;
  uint32_t cpb_size_scale = 0;
  std::vector<uint32_t> bit_rate_value_minus1;
  std::vector<uint32_t> cpb_size_value_minus1;
  std::vector<bool> cbr_flag;
  uint32_t initial_cpb_removal_delay_length_minus1 = 23;  // E.2.2 inferred defaults when absent
  uint32_t cpb_removal_delay_length_minus1 = 23;
  uint32_t dpb_output_delay_length_minus1 = 23;
  uint32_t time_offset_length = 24;
  // derived (E.2.2)
  std::vector<uint64_t> BitRate;
  std::vector<uint64_t> CpbSize;
  BitSpan span{0, 0};
};

/// vui_parameters() (E.1.1), row 4. Fields follow the syntax-table order, not the padding-optimal one.
struct VuiParameters {  // NOLINT(clang-analyzer-optin.performance.Padding)
  bool aspect_ratio_info_present_flag = false;
  uint32_t aspect_ratio_idc = 0;
  uint32_t sar_width = 0;
  uint32_t sar_height = 0;
  bool overscan_info_present_flag = false;
  bool overscan_appropriate_flag = false;
  bool video_signal_type_present_flag = false;
  uint32_t video_format = 5;
  bool video_full_range_flag = false;
  bool colour_description_present_flag = false;
  uint32_t colour_primaries = 2;
  uint32_t transfer_characteristics = 2;
  uint32_t matrix_coefficients = 2;
  bool chroma_loc_info_present_flag = false;
  uint32_t chroma_sample_loc_type_top_field = 0;
  uint32_t chroma_sample_loc_type_bottom_field = 0;
  bool timing_info_present_flag = false;
  uint32_t num_units_in_tick = 0;
  uint32_t time_scale = 0;
  bool fixed_frame_rate_flag = false;
  bool nal_hrd_parameters_present_flag = false;
  HrdParameters nal_hrd;
  bool vcl_hrd_parameters_present_flag = false;
  HrdParameters vcl_hrd;
  bool low_delay_hrd_flag = false;
  bool pic_struct_present_flag = false;
  bool bitstream_restriction_flag = false;
  bool motion_vectors_over_pic_boundaries_flag = true;
  uint32_t max_bytes_per_pic_denom = 2;
  uint32_t max_bits_per_mb_denom = 1;
  uint32_t log2_max_mv_length_horizontal = 15;
  uint32_t log2_max_mv_length_vertical = 15;
  uint32_t max_num_reorder_frames = 0;
  uint32_t max_dec_frame_buffering = 0;
  // derived
  double frame_rate = 0.0;               // time_scale / (2 * num_units_in_tick) (E.2.1); 0 when no timing info
  bool CpbDpbDelaysPresentFlag = false;  // nal or vcl HRD present (E.2.1); needed by pic_timing (row 9)
  uint8_t chroma_siting = 0;             // ovt_ColorDescription.chroma_siting from chroma_sample_loc_type_top_field
  BitSpan span{0, 0};
};

/// seq_parameter_set_data() (§7.3.2.1.1), row 2, plus the derived table (§7.4.2.1.1, Table A-1).
struct Sps {
  uint32_t profile_idc = 0;
  bool constraint_set0_flag = false;
  bool constraint_set1_flag = false;
  bool constraint_set2_flag = false;
  bool constraint_set3_flag = false;
  bool constraint_set4_flag = false;
  bool constraint_set5_flag = false;
  uint32_t reserved_zero_2bits = 0;
  uint32_t level_idc = 0;
  uint32_t seq_parameter_set_id = 0;
  bool high_profile_syntax = false;  // profile_idc in the row-2 set: the chroma/bit-depth/scaling block is present
  uint32_t chroma_format_idc = 1;
  bool separate_colour_plane_flag = false;
  uint32_t bit_depth_luma_minus8 = 0;
  uint32_t bit_depth_chroma_minus8 = 0;
  bool qpprime_y_zero_transform_bypass_flag = false;
  bool seq_scaling_matrix_present_flag = false;
  std::array<ScalingList, 12> scaling_list{};  // seq_scaling_list_present_flag[i] is scaling_list[i].present
  uint32_t log2_max_frame_num_minus4 = 0;
  uint32_t pic_order_cnt_type = 0;
  uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;
  bool delta_pic_order_always_zero_flag = false;
  int32_t offset_for_non_ref_pic = 0;
  int32_t offset_for_top_to_bottom_field = 0;
  uint32_t num_ref_frames_in_pic_order_cnt_cycle = 0;
  std::vector<int32_t> offset_for_ref_frame;
  uint32_t max_num_ref_frames = 0;
  bool gaps_in_frame_num_value_allowed_flag = false;
  uint32_t pic_width_in_mbs_minus1 = 0;
  uint32_t pic_height_in_map_units_minus1 = 0;
  bool frame_mbs_only_flag = true;
  bool mb_adaptive_frame_field_flag = false;
  bool direct_8x8_inference_flag = false;
  bool frame_cropping_flag = false;
  uint32_t frame_crop_left_offset = 0;
  uint32_t frame_crop_right_offset = 0;
  uint32_t frame_crop_top_offset = 0;
  uint32_t frame_crop_bottom_offset = 0;
  bool vui_parameters_present_flag = false;
  VuiParameters vui;
  // derived
  std::string_view profile_name;  // Annex A name (static storage); "unknown" for an unlisted profile_idc
  std::string_view level_name;    // "1b", "1.0" … "6.2" (static storage); empty for an unlisted level_idc
  uint32_t ChromaArrayType = 1;
  uint32_t SubWidthC = 2;  // 0 when ChromaArrayType == 0 (n/a)
  uint32_t SubHeightC = 2;
  uint32_t BitDepthY = 8;
  uint32_t BitDepthC = 8;
  uint32_t QpBdOffsetY = 0;
  uint32_t QpBdOffsetC = 0;
  uint32_t MaxFrameNum = 16;
  uint32_t MaxPicOrderCntLsb = 16;  // pic_order_cnt_type 0 only
  int64_t ExpectedDeltaPerPicOrderCntCycle = 0;
  uint32_t PicWidthInMbs = 1;
  uint32_t PicWidthInSamplesL = 16;
  uint32_t PicHeightInMapUnits = 1;
  uint64_t PicSizeInMapUnits = 1;  // PicWidthInMbs * PicHeightInMapUnits (up to 2^32 under the #388 caps)
  uint32_t FrameHeightInMbs = 1;
  uint32_t CropUnitX = 1;
  uint32_t CropUnitY = 1;
  int32_t crop_x = 0;  // crop rectangle in luma samples (§7.4.2.1.1)
  int32_t crop_y = 0;
  int32_t crop_width = 16;
  int32_t crop_height = 16;
  uint32_t MaxDpbMbs = 0;                    // Table A-1; 0 for an unlisted level_idc
  uint32_t MaxDpbFrames = 0;                 // Min(MaxDpbMbs / (PicWidthInMbs * FrameHeightInMbs), 16)
  ovt_ColorDescription color_description{};  // row 4 candidate: primaries/transfer/matrix/range/siting + format/depth
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 3. Reads one scaling_list() of `size` (16 or 64) entries into `out` (sets size, delta_scale, list and
/// UseDefaultScalingMatrixFlag). Node `scaling_list`, opened as array element `index`.
void parse_scaling_list(BitReader& br, SyntaxBuilder& b, ScalingList& out, uint32_t size, int index);

/// Row 5. Node `hrd_parameters`. Throws ParseError when cpb_cnt_minus1 > 31.
HrdParameters parse_hrd_parameters(BitReader& br, SyntaxBuilder& b);

/// Row 4. Node `vui_parameters`.
VuiParameters parse_vui_parameters(BitReader& br, SyntaxBuilder& b);

/// Row 2 without the RBSP wrapper: seq_parameter_set_data(). Node `seq_parameter_set_data`. Used by parse_sps()
/// and by subset_seq_parameter_set_rbsp() (row 22, WI-1.109).
Sps parse_seq_parameter_set_data(BitReader& br, SyntaxBuilder& b);

/// seq_parameter_set_rbsp() (§7.3.2.1): row 2 followed by rbsp_trailing_bits(). Node `seq_parameter_set_rbsp`.
/// The reader stands after the NAL header (parse_nal_header()).
Sps parse_sps(BitReader& br, SyntaxBuilder& b);

/// Annex A profile name for profile_idc (+ constraint flags for the constrained/intra variants).
[[nodiscard]] std::string_view profile_name(uint32_t profile_idc, bool constraint_set1_flag, bool constraint_set3_flag,
                                            bool constraint_set4_flag, bool constraint_set5_flag) noexcept;

/// Table A-1 MaxDpbMbs for level_idc (level 1b: level_idc 9, or 11 with constraint_set3_flag on Baseline, Main
/// and Extended, §A.3.1); 0 when the level is not listed. `name` receives "1b", "1.0" … "6.2" or stays empty.
[[nodiscard]] uint32_t max_dpb_mbs(uint32_t profile_idc, uint32_t level_idc, bool constraint_set3_flag,
                                   std::string_view& name) noexcept;

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_SPS_HPP
