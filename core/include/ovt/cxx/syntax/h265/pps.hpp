// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 pic_parameter_set_rbsp() (H.265 §7.3.2.3): typed struct, parser and the ActivePps helper that derives the
// tile tables against the SPS at activation (chapter 7 §7.3.5 row 8, §7.3.7, §7.3.8; WI-1.112). C++ helper header
// (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_SYNTAX_H265_PPS_HPP
#define OVT_CXX_SYNTAX_H265_PPS_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/scaling_list.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// pps_range_extension() (§7.3.2.3.2).
struct PpsRangeExtension {
  uint32_t log2_max_transform_skip_block_size_minus2 = 0;
  bool cross_component_prediction_enabled_flag = false;
  bool chroma_qp_offset_list_enabled_flag = false;
  uint32_t diff_cu_chroma_qp_offset_depth = 0;
  uint32_t chroma_qp_offset_list_len_minus1 = 0;
  std::array<int32_t, 6> cb_qp_offset_list{};
  std::array<int32_t, 6> cr_qp_offset_list{};
  uint32_t log2_sao_offset_scale_luma = 0;
  uint32_t log2_sao_offset_scale_chroma = 0;
};

/// pps_scc_extension() (§7.3.2.3.3).
struct PpsSccExtension {
  bool pps_curr_pic_ref_enabled_flag = false;
  bool residual_adaptive_colour_transform_enabled_flag = false;
  bool pps_slice_act_qp_offsets_present_flag = false;
  int32_t pps_act_y_qp_offset_plus5 = 0;
  int32_t pps_act_cb_qp_offset_plus5 = 0;
  int32_t pps_act_cr_qp_offset_plus3 = 0;
  bool pps_palette_predictor_initializers_present_flag = false;
  uint32_t pps_num_palette_predictor_initializers = 0;
  bool monochrome_palette_flag = false;
  uint32_t luma_bit_depth_entry_minus8 = 0;
  uint32_t chroma_bit_depth_entry_minus8 = 0;
  std::array<std::vector<uint32_t>, 3> pps_palette_predictor_initializer;  // [comp][i]
};

/// Typed pic_parameter_set_rbsp() (§7.3.7). The tile tables need the SPS and live in ActivePps (§7.3.8).
struct Pps {
  uint8_t pps_pic_parameter_set_id = 0;
  uint8_t pps_seq_parameter_set_id = 0;
  bool dependent_slice_segments_enabled_flag = false;
  bool output_flag_present_flag = false;
  uint8_t num_extra_slice_header_bits = 0;
  bool sign_data_hiding_enabled_flag = false;
  bool cabac_init_present_flag = false;
  uint32_t num_ref_idx_l0_default_active_minus1 = 0;
  uint32_t num_ref_idx_l1_default_active_minus1 = 0;
  int32_t init_qp_minus26 = 0;
  bool constrained_intra_pred_flag = false;
  bool transform_skip_enabled_flag = false;
  bool cu_qp_delta_enabled_flag = false;
  uint32_t diff_cu_qp_delta_depth = 0;
  int32_t pps_cb_qp_offset = 0;
  int32_t pps_cr_qp_offset = 0;
  bool pps_slice_chroma_qp_offsets_present_flag = false;
  bool weighted_pred_flag = false;
  bool weighted_bipred_flag = false;
  bool transquant_bypass_enabled_flag = false;
  bool tiles_enabled_flag = false;
  bool entropy_coding_sync_enabled_flag = false;
  uint32_t num_tile_columns_minus1 = 0;
  uint32_t num_tile_rows_minus1 = 0;
  bool uniform_spacing_flag = true;                   // inferred 1 when tiles are off
  std::vector<uint32_t> column_width_minus1;          // num_tile_columns_minus1 entries when !uniform_spacing_flag
  std::vector<uint32_t> row_height_minus1;            // num_tile_rows_minus1 entries
  bool loop_filter_across_tiles_enabled_flag = true;  // inferred 1
  bool pps_loop_filter_across_slices_enabled_flag = false;
  bool deblocking_filter_control_present_flag = false;
  bool deblocking_filter_override_enabled_flag = false;
  bool pps_deblocking_filter_disabled_flag = false;
  int32_t pps_beta_offset_div2 = 0;
  int32_t pps_tc_offset_div2 = 0;
  bool pps_scaling_list_data_present_flag = false;
  std::optional<ScalingListData> scaling_list;
  bool lists_modification_present_flag = false;
  uint32_t log2_parallel_merge_level_minus2 = 0;
  bool slice_segment_header_extension_present_flag = false;
  bool pps_extension_present_flag = false;
  bool pps_range_extension_flag = false;
  bool pps_multilayer_extension_flag = false;
  bool pps_3d_extension_flag = false;
  bool pps_scc_extension_flag = false;
  uint8_t pps_extension_4bits = 0;
  PpsRangeExtension range_extension;  // zeros unless pps_range_extension_flag
  PpsSccExtension scc_extension;      // zeros unless pps_scc_extension_flag
  bool opaque_tail = false;  // pps_multilayer_extension(), pps_3d_extension() or pps_extension_data_flag kept opaque
  // derived that need no SPS
  int32_t SliceQpY_default = 26;  // 26 + init_qp_minus26
  uint32_t Log2ParMrgLevel = 2;
  uint32_t Log2MaxTransformSkipSize = 2;
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;  // set by the owning Unit
};

/// Parses pic_parameter_set_rbsp() after the NAL header into the node `pic_parameter_set_rbsp`.
/// pps_pic_parameter_set_id > 63 throws ParseError SYN-H265-0010. Elements that size a list, become a shift count
/// or enter QP arithmetic (ids, tile counts, num_ref_idx defaults, init_qp_minus26, the QP offsets, the log2 and
/// depth fields, the palette sizes) throw ParseError SYN-H265-0007 outside their §7.4.3.3 ranges; the other range
/// violations are Diagnostic Error SYN-H265-0007 and parsing continues.
Pps parse_pps(BitReader& br, SyntaxBuilder& b);

/// A PPS activated against its SPS (§7.3.8): the §6.5.1 tile tables and the §7.4.3.3 derived variables.
struct ActivePps {
  const Pps* pps = nullptr;
  const Sps* sps = nullptr;
  std::vector<uint32_t> colBd;  // num_tile_columns_minus1 + 2 entries, in CTBs
  std::vector<uint32_t> rowBd;  // num_tile_rows_minus1 + 2 entries
  std::vector<uint32_t> ColumnWidthInLumaSamples;
  std::vector<uint32_t> RowHeightInLumaSamples;
  std::vector<uint32_t> CtbAddrRsToTs;  // PicSizeInCtbsY entries
  std::vector<uint32_t> CtbAddrTsToRs;
  std::vector<uint32_t> TileId;  // indexed by tile-scan address
  uint32_t Log2MinCuQpDeltaSize = 0;
  uint32_t Log2MinCuChromaQpOffsetSize = 0;
  uint32_t Log2ParMrgLevel = 2;
  uint32_t Log2MaxTransformSkipSize = 2;
  int32_t SliceQpY_default = 26;
  /// Conformance problems found at activation (tile widths that do not fit the picture, …), as
  /// (code, message) pairs for the caller to attach to the activating slice.
  struct Issue {
    std::string code;
    std::string message;
  };
  std::vector<Issue> issues;
};

/// Activates `pps` against `sps` (same lifetime requirements as the pointers it stores): computes colBd[], rowBd[],
/// CtbAddrRsToTs[], CtbAddrTsToRs[], TileId[] and the derived variables of §7.4.3.3. Inconsistent explicit tile
/// sizes are clamped and reported in `issues` (SYN-H265-0007); a picture beyond the level 6.2 MaxLumaPs or more
/// tiles than CTBs leaves the tables empty with one issue.
[[nodiscard]] ActivePps activate_pps(const Pps& pps, const Sps& sps);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_PPS_HPP
