// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 subset_seq_parameter_set_rbsp() (§7.3.2.1.3) with the SVC (G.7.3.2.1.4), MVC (H.7.3.2.1.4) and MVCD
// (I.7.3.2.1.5) extensions and their VUI extensions (G.14.1, H.14.1, I.14.1): chapter 7 §7.3.4 row 22 and §7.3.7,
// WI-1.109. Clause letters follow H.264 (08/2021). Headers only: OVT parses these so that SVC / MVC streams show a
// complete tree; block analysis of enhancement layers is out of scope. C++ helper header (D22): semi-public, no ABI
// promise.

#ifndef OVT_CXX_SYNTAX_H264_SUBSET_SPS_HPP
#define OVT_CXX_SYNTAX_H264_SUBSET_SPS_HPP

#include <cstdint>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {
class SyntaxNodeBuffer;
}

namespace ovt::syntax::h264 {

/// seq_parameter_set_svc_extension() (G.7.3.2.1.4). Absent elements hold their inferred values (G.7.4.2.1.4).
struct SvcSpsExtension {  // NOLINT(clang-analyzer-optin.performance.Padding): syntax-table order
  bool inter_layer_deblocking_filter_control_present_flag = false;
  uint32_t extended_spatial_scalability_idc = 0;
  bool chroma_phase_x_plus1_flag = true;
  uint32_t chroma_phase_y_plus1 = 1;
  bool seq_ref_layer_chroma_phase_x_plus1_flag = true;  // inferred: chroma_phase_x_plus1_flag
  uint32_t seq_ref_layer_chroma_phase_y_plus1 = 1;      // inferred: chroma_phase_y_plus1
  int32_t seq_scaled_ref_layer_left_offset = 0;
  int32_t seq_scaled_ref_layer_top_offset = 0;
  int32_t seq_scaled_ref_layer_right_offset = 0;
  int32_t seq_scaled_ref_layer_bottom_offset = 0;
  bool seq_tcoeff_level_prediction_flag = false;
  bool adaptive_tcoeff_level_prediction_flag = false;
  bool slice_header_restriction_flag = false;
};

/// One entry i of svc_vui_parameters_extension() (vui_ext_*), mvc_vui_parameters_extension() (vui_mvc_*) or
/// mvcd_vui_parameters_extension() (vui_mvcd_*). The three share everything after the operation point fields.
struct VuiExtensionEntry {     // NOLINT(clang-analyzer-optin.performance.Padding): syntax-table order
  uint32_t dependency_id = 0;  // SVC only
  uint32_t quality_id = 0;     // SVC only
  uint32_t temporal_id = 0;
  std::vector<uint32_t> view_id;   // MVC / MVCD: the target output views
  std::vector<bool> depth_flag;    // MVCD only
  std::vector<bool> texture_flag;  // MVCD only
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
};

/// One operation point j of level value i in seq_parameter_set_mvc_extension() or seq_parameter_set_mvcd_extension():
/// applicable_op_*[i][j].
struct MvcApplicableOp {
  uint32_t temporal_id = 0;
  std::vector<uint32_t> target_view_id;   // applicable_op_target_view_id[i][j][k]
  std::vector<bool> depth_flag;           // MVCD only: applicable_op_depth_flag[i][j][k]
  std::vector<bool> texture_flag;         // MVCD only: applicable_op_texture_flag[i][j][k]
  uint32_t num_views_minus1 = 0;          // MVC only
  uint32_t num_texture_views_minus1 = 0;  // MVCD only
  uint32_t num_depth_views = 0;           // MVCD only
};

/// One level value i of seq_parameter_set_mvc_extension().
struct MvcLevelValue {
  uint32_t level_idc = 0;
  std::vector<MvcApplicableOp> ops;
};

/// seq_parameter_set_mvc_extension() (H.7.3.2.1.4) or seq_parameter_set_mvcd_extension() (I.7.3.2.1.5). The per-view
/// lists are indexed by view order index i; entry 0 (the base view) is empty. In the MVCD extension the reference lists
/// apply to depth views and are empty for a view without depth (I.7.4.2.1.4).
struct MvcSpsExtension {  // NOLINT(clang-analyzer-optin.performance.Padding): syntax-table order
  uint32_t num_views_minus1 = 0;
  std::vector<uint32_t> view_id;
  std::vector<bool> depth_view_present_flag;    // MVCD only
  std::vector<bool> texture_view_present_flag;  // MVCD only
  std::vector<std::vector<uint32_t>> anchor_ref_l0;
  std::vector<std::vector<uint32_t>> anchor_ref_l1;
  std::vector<std::vector<uint32_t>> non_anchor_ref_l0;
  std::vector<std::vector<uint32_t>> non_anchor_ref_l1;
  std::vector<MvcLevelValue> level_values;
  // profile_idc 134 (MFC High) only.
  uint32_t mfc_format_idc = 0;
  bool default_grid_position_flag = false;
  uint32_t view0_grid_position_x = 0;
  uint32_t view0_grid_position_y = 0;
  uint32_t view1_grid_position_x = 0;
  uint32_t view1_grid_position_y = 0;
  bool rpu_filter_enabled_flag = false;
  bool rpu_field_processing_flag = false;
  uint32_t NumDepthViews = 0;  // derived (MVCD, I.7.3.2.1.5)
};

/// Which extension follows seq_parameter_set_data() in a subset SPS (selected by profile_idc, §7.3.2.1.3).
enum class SubsetSpsExtension {
  None,       // a profile_idc without an extension in this syntax structure
  Svc,        // 83, 86: seq_parameter_set_svc_extension()
  Mvc,        // 118, 128, 134: seq_parameter_set_mvc_extension()
  Mvcd,       // 135, 138: seq_parameter_set_mvcd_extension()
  Mvcd3dAvc,  // 139: seq_parameter_set_mvcd_extension(), then seq_parameter_set_3davc_extension() kept as bytes
};

/// subset_seq_parameter_set_rbsp(), row 22 (nal_unit_type 15).
struct SubsetSps {
  Sps sps;  // seq_parameter_set_data(): what ParameterSets::subset_sps holds (H.7.4.1.2.1)
  SubsetSpsExtension extension = SubsetSpsExtension::None;
  SvcSpsExtension svc;
  bool svc_vui_parameters_present_flag = false;
  std::vector<VuiExtensionEntry> svc_vui;
  bool bit_equal_to_one = true;
  MvcSpsExtension mvc;  // the MVC or the MVCD extension
  bool mvc_vui_parameters_present_flag = false;
  bool mvcd_vui_parameters_present_flag = false;
  std::vector<VuiExtensionEntry> mvcd_vui;
  bool texture_vui_parameters_present_flag = false;
  std::vector<VuiExtensionEntry> mvc_vui;  // the MVC VUI, or the MVCD texture sub-bitstream VUI
  bool additional_extension2_flag = false;
  uint64_t additional_extension2_data_bits = 0;  // derived: number of additional_extension2_data_flag read
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 22. Node `subset_seq_parameter_set_rbsp`. The reader stands after the NAL header. Throws ParseError
/// SYN-H264-0002 on a loop bound out of range (§7.3.1 rule 3). For profile_idc 139 the rest of the RBSP after the MVCD
/// extension (seq_parameter_set_3davc_extension() onward) is kept as the opaque node `subset_sps_extension` with Info
/// SYN-H264-0071 (#552); parsing it belongs to WI-4.111.
SubsetSps parse_subset_sps(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_SUBSET_SPS_HPP
