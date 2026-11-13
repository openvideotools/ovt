// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 seq_parameter_set_rbsp() (H.265 §7.3.2.2): typed struct and parser (chapter 7 §7.3.5 row 4, §7.3.7;
// WI-1.112). C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_SYNTAX_H265_SPS_HPP
#define OVT_CXX_SYNTAX_H265_SPS_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/profile_tier_level.hpp"
#include "ovt/cxx/syntax/h265/scaling_list.hpp"
#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/h265/vui.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// sps_range_extension() (§7.3.2.2.2).
struct SpsRangeExtension {
  bool transform_skip_rotation_enabled_flag = false;
  bool transform_skip_context_enabled_flag = false;
  bool implicit_rdpcm_enabled_flag = false;
  bool explicit_rdpcm_enabled_flag = false;
  bool extended_precision_processing_flag = false;
  bool intra_smoothing_disabled_flag = false;
  bool high_precision_offsets_enabled_flag = false;
  bool persistent_rice_adaptation_enabled_flag = false;
  bool cabac_bypass_alignment_enabled_flag = false;
};

/// sps_scc_extension() (§7.3.2.2.3).
struct SpsSccExtension {
  bool sps_curr_pic_ref_enabled_flag = false;
  bool palette_mode_enabled_flag = false;
  uint32_t palette_max_size = 0;
  uint32_t delta_palette_max_predictor_size = 0;
  bool sps_palette_predictor_initializers_present_flag = false;
  uint32_t sps_num_palette_predictor_initializers_minus1 = 0;
  std::array<std::vector<uint32_t>, 3> sps_palette_predictor_initializer;  // [comp][i]
  uint8_t motion_vector_resolution_control_idc = 0;
  bool intra_boundary_filtering_disabled_flag = false;
  // derived
  uint32_t PaletteMaxPredictorSize = 0;
};

/// Typed seq_parameter_set_rbsp() (§7.3.7). Derived variables follow §7.4.3.2.1. Fields keep the syntax order of
/// §7.3.2.2 for readability, not the padding-optimal one.
struct Sps {  // NOLINT(clang-analyzer-optin.performance.Padding)
  uint8_t sps_video_parameter_set_id = 0;
  uint8_t sps_max_sub_layers_minus1 = 0;
  bool sps_temporal_id_nesting_flag = false;
  ProfileTierLevel profile_tier_level;
  uint8_t sps_seq_parameter_set_id = 0;
  uint8_t chroma_format_idc = 1;
  bool separate_colour_plane_flag = false;
  uint32_t pic_width_in_luma_samples = 0;
  uint32_t pic_height_in_luma_samples = 0;
  bool conformance_window_flag = false;
  uint32_t conf_win_left_offset = 0;
  uint32_t conf_win_right_offset = 0;
  uint32_t conf_win_top_offset = 0;
  uint32_t conf_win_bottom_offset = 0;
  uint8_t bit_depth_luma_minus8 = 0;
  uint8_t bit_depth_chroma_minus8 = 0;
  uint8_t log2_max_pic_order_cnt_lsb_minus4 = 0;
  bool sps_sub_layer_ordering_info_present_flag = false;
  std::array<SubLayerOrderingInfo, 7> sub_layer_ordering_info;  // sps_max_dec_pic_buffering_minus1[i] etc.
  uint32_t log2_min_luma_coding_block_size_minus3 = 0;
  uint32_t log2_diff_max_min_luma_coding_block_size = 0;
  uint32_t log2_min_luma_transform_block_size_minus2 = 0;
  uint32_t log2_diff_max_min_luma_transform_block_size = 0;
  uint32_t max_transform_hierarchy_depth_inter = 0;
  uint32_t max_transform_hierarchy_depth_intra = 0;
  bool scaling_list_enabled_flag = false;
  bool sps_scaling_list_data_present_flag = false;
  ScalingListData scaling_list;  // parsed, default (Table 7-5/7-6) or flat, per §7.4.5
  bool amp_enabled_flag = false;
  bool sample_adaptive_offset_enabled_flag = false;
  bool pcm_enabled_flag = false;
  uint8_t pcm_sample_bit_depth_luma_minus1 = 0;
  uint8_t pcm_sample_bit_depth_chroma_minus1 = 0;
  uint32_t log2_min_pcm_luma_coding_block_size_minus3 = 0;
  uint32_t log2_diff_max_min_pcm_luma_coding_block_size = 0;
  bool pcm_loop_filter_disabled_flag = false;
  uint32_t num_short_term_ref_pic_sets = 0;
  std::vector<StRefPicSet> st_ref_pic_set;  // num_short_term_ref_pic_sets entries
  bool long_term_ref_pics_present_flag = false;
  uint32_t num_long_term_ref_pics_sps = 0;
  std::array<uint32_t, 32> lt_ref_pic_poc_lsb_sps{};
  std::array<bool, 32> used_by_curr_pic_lt_sps_flag{};
  bool sps_temporal_mvp_enabled_flag = false;
  bool strong_intra_smoothing_enabled_flag = false;
  bool vui_parameters_present_flag = false;
  std::optional<VuiParameters> vui;
  bool sps_extension_present_flag = false;
  bool sps_range_extension_flag = false;
  bool sps_multilayer_extension_flag = false;
  bool sps_3d_extension_flag = false;
  bool sps_scc_extension_flag = false;
  uint8_t sps_extension_4bits = 0;
  SpsRangeExtension range_extension;                // zeros unless sps_range_extension_flag
  bool inter_view_mv_vert_constraint_flag = false;  // sps_multilayer_extension()
  SpsSccExtension scc_extension;                    // zeros unless sps_scc_extension_flag
  bool opaque_tail = false;  // a multilayer SPS, sps_3d_extension() or sps_extension_data_flag kept as opaque bytes

  // derived (§7.4.3.2.1)
  uint8_t ChromaArrayType = 1;
  uint8_t SubWidthC = 2;
  uint8_t SubHeightC = 2;
  uint32_t MinCbLog2SizeY = 3;
  uint32_t CtbLog2SizeY = 4;
  uint32_t MinCbSizeY = 8;
  uint32_t CtbSizeY = 16;
  uint32_t PicWidthInMinCbsY = 0;
  uint32_t PicWidthInCtbsY = 0;
  uint32_t PicHeightInMinCbsY = 0;
  uint32_t PicHeightInCtbsY = 0;
  uint32_t PicSizeInMinCbsY = 0;
  uint32_t PicSizeInCtbsY = 0;
  uint64_t PicSizeInSamplesY = 0;
  uint32_t PicWidthInSamplesC = 0;
  uint32_t PicHeightInSamplesC = 0;
  uint32_t MinTbLog2SizeY = 2;
  uint32_t MaxTbLog2SizeY = 2;
  uint32_t BitDepthY = 8;
  uint32_t BitDepthC = 8;
  uint32_t QpBdOffsetY = 0;
  uint32_t QpBdOffsetC = 0;
  uint32_t MaxPicOrderCntLsb = 16;
  uint32_t PcmBitDepthY = 0;
  uint32_t PcmBitDepthC = 0;
  uint32_t Log2MinIpcmCbSizeY = 0;
  uint32_t Log2MaxIpcmCbSizeY = 0;
  int32_t CoeffMinY = -32768;
  int32_t CoeffMinC = -32768;
  int32_t CoeffMaxY = 32767;
  int32_t CoeffMaxC = 32767;
  uint32_t WpOffsetBdShiftY = 0;
  uint32_t WpOffsetBdShiftC = 0;
  uint32_t WpOffsetHalfRangeY = 128;
  uint32_t WpOffsetHalfRangeC = 128;
  struct CropRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
  } conformance_crop;             // luma samples
  uint32_t MaxDpbSizeMinus1 = 0;  // sps_max_dec_pic_buffering_minus1[HighestTid = sps_max_sub_layers_minus1]
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;  // set by the owning Unit
};

/// Parses seq_parameter_set_rbsp() after the NAL header into the node `seq_parameter_set_rbsp`. `nuh_layer_id`
/// is the header's value: an SPS with nuh_layer_id > 0 (F.7.3.2.2.1) is kept opaque after
/// sps_video_parameter_set_id in phase 1, with a Diagnostic (§7.3.5 row 4). Range violations that would size
/// later reads throw ParseError SYN-H265-0007; others are Diagnostic Error SYN-H265-0007 and parsing continues.
Sps parse_sps(BitReader& br, SyntaxBuilder& b, uint8_t nuh_layer_id = 0);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_SPS_HPP
