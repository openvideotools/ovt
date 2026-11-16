// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 subset_seq_parameter_set_rbsp() (chapter 7 §7.3.4 row 22, WI-1.109). The outer structure follows row 22
// (H.264 (08/2021) §7.3.2.1.3); seq_parameter_set_mvc_extension() follows H.7.3.2.1.4 and is checked against
// GStreamer gst_h264_parser_parse_subset_sps() (acceptance test 3). The SVC (G.7.3.2.1.4, G.14.1), MVC VUI (H.14.1)
// and MVCD (I.7.3.2.1.5, I.14.1) structures follow the syntax tables line by line; clause letters are those of
// H.264 (08/2021), which the 06/2026 edition renumbers F, G and H.

#include "ovt/cxx/syntax/h264/subset_sps.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

// H.7.4.2.1.4 value ranges: view ids, view counts and operation-point counts are 0..1023, the level value count is
// 0..63 and each reference list holds at most Min(15, num_views_minus1) views. The VUI extensions bound their entry
// and target view counts to 0..1023 as well (G.14.2, H.14.2, I.14.2).
constexpr int64_t kMaxViewIndex = 1023;
constexpr int64_t kMaxLevelValuesMinus1 = 63;
constexpr int64_t kMaxInterViewRefs = 15;
// G.7.4.2.1.4: seq_scaled_ref_layer_*_offset in -2^15..2^15-1.
constexpr int64_t kMaxScaledOffset = 32767;

enum class Ext { Svc, Mvc, Mvcd };

// svc_vui_parameters_extension() (G.14.1), mvc_vui_parameters_extension() (H.14.1) and
// mvcd_vui_parameters_extension() (I.14.1) differ only in the operation point fields at the start of an entry and in
// the element name prefix.
struct VuiExtNames {
  std::string_view node;
  std::string_view ref;
  std::string_view prefix;
  [[nodiscard]] std::string operator()(std::string_view n) const { return std::string(prefix).append(n); }
};

VuiExtNames vui_ext_names(Ext ext) {
  switch (ext) {
    case Ext::Svc:
      return {"svc_vui_parameters_extension", "H.264 G.14.1", "vui_ext_"};
    case Ext::Mvc:
      return {"mvc_vui_parameters_extension", "H.264 H.14.1", "vui_mvc_"};
    case Ext::Mvcd:
      break;
  }
  return {"mvcd_vui_parameters_extension", "H.264 I.14.1", "vui_mvcd_"};
}

// The operation point fields of entry i: SVC dependency / quality / temporal id, or MVC / MVCD temporal id and target
// output views (with MVCD depth / texture flags).
void read_vui_op_point(BitReader& br, SyntaxBuilder& b, Ext ext, const VuiExtNames& name, VuiExtensionEntry& e) {
  if (ext == Ext::Svc) {
    e.dependency_id = static_cast<uint32_t>(b.read_u(name("dependency_id"), 3));
    e.quality_id = static_cast<uint32_t>(b.read_u(name("quality_id"), 4));
    e.temporal_id = static_cast<uint32_t>(b.read_u(name("temporal_id"), 3));
    return;
  }
  e.temporal_id = static_cast<uint32_t>(b.read_u(name("temporal_id"), 3));
  const std::string views_name = name("num_target_output_views_minus1");
  const uint32_t views_minus1 = b.read_ue(views_name);
  detail::require_range(br, views_name, views_minus1, 0, kMaxViewIndex);
  for (uint32_t j = 0; j <= views_minus1; ++j) {
    b.begin_array_element(name.node, static_cast<int>(j));
    e.view_id.push_back(b.read_ue(name("view_id")));
    detail::check_range(b, name("view_id"), e.view_id.back(), 0, kMaxViewIndex, name.ref);
    if (ext == Ext::Mvcd) {
      e.depth_flag.push_back(b.read_flag(name("depth_flag")));
      e.texture_flag.push_back(b.read_flag(name("texture_flag")));
    }
    b.end_array_element();
  }
}

// The fields shared by all three: timing, NAL / VCL HRD, low delay and pic_struct presence.
void read_vui_timing_hrd(BitReader& br, SyntaxBuilder& b, const VuiExtNames& name, VuiExtensionEntry& e) {
  e.timing_info_present_flag = b.read_flag(name("timing_info_present_flag"));
  if (e.timing_info_present_flag) {
    e.num_units_in_tick = static_cast<uint32_t>(b.read_u(name("num_units_in_tick"), 32));
    e.time_scale = static_cast<uint32_t>(b.read_u(name("time_scale"), 32));
    e.fixed_frame_rate_flag = b.read_flag(name("fixed_frame_rate_flag"));
  }
  e.nal_hrd_parameters_present_flag = b.read_flag(name("nal_hrd_parameters_present_flag"));
  if (e.nal_hrd_parameters_present_flag) {
    e.nal_hrd = parse_hrd_parameters(br, b);
  }
  e.vcl_hrd_parameters_present_flag = b.read_flag(name("vcl_hrd_parameters_present_flag"));
  if (e.vcl_hrd_parameters_present_flag) {
    e.vcl_hrd = parse_hrd_parameters(br, b);
  }
  if (e.nal_hrd_parameters_present_flag || e.vcl_hrd_parameters_present_flag) {
    e.low_delay_hrd_flag = b.read_flag(name("low_delay_hrd_flag"));
  }
  e.pic_struct_present_flag = b.read_flag(name("pic_struct_present_flag"));
}

std::vector<VuiExtensionEntry> parse_vui_extension(BitReader& br, SyntaxBuilder& b, Ext ext) {
  const VuiExtNames name = vui_ext_names(ext);
  auto scope = b.scoped(name.node, name.ref);
  const std::string count_name = name(ext == Ext::Svc ? "num_entries_minus1" : "num_ops_minus1");
  const uint32_t count_minus1 = b.read_ue(count_name);
  detail::require_range(br, count_name, count_minus1, 0, kMaxViewIndex);
  std::vector<VuiExtensionEntry> entries;
  for (uint32_t i = 0; i <= count_minus1; ++i) {
    VuiExtensionEntry e;
    b.begin_array_element(name.node, static_cast<int>(i));
    read_vui_op_point(br, b, ext, name, e);
    read_vui_timing_hrd(br, b, name, e);
    b.end_array_element();
    entries.push_back(std::move(e));
  }
  return entries;
}

// seq_parameter_set_svc_extension() (G.7.3.2.1.4).
void parse_svc_extension(SyntaxBuilder& b, uint32_t chroma_array_type, SvcSpsExtension& v) {
  auto scope = b.scoped("seq_parameter_set_svc_extension", "H.264 G.7.3.2.1.4");
  v.inter_layer_deblocking_filter_control_present_flag =
      b.read_flag("inter_layer_deblocking_filter_control_present_flag");
  v.extended_spatial_scalability_idc = static_cast<uint32_t>(b.read_u("extended_spatial_scalability_idc", 2));
  if (v.extended_spatial_scalability_idc == 3) {
    detail::check_range(b, "extended_spatial_scalability_idc", 3, 0, 2, "H.264 G.7.4.2.1.4");
  }
  if (chroma_array_type == 1 || chroma_array_type == 2) {
    v.chroma_phase_x_plus1_flag = b.read_flag("chroma_phase_x_plus1_flag");
  }
  if (chroma_array_type == 1) {
    v.chroma_phase_y_plus1 = static_cast<uint32_t>(b.read_u("chroma_phase_y_plus1", 2));
    detail::check_range(b, "chroma_phase_y_plus1", v.chroma_phase_y_plus1, 0, 2, "H.264 G.7.4.2.1.4");
  }
  // G.7.4.2.1.4: when absent, the reference layer chroma phases equal the current ones.
  v.seq_ref_layer_chroma_phase_x_plus1_flag = v.chroma_phase_x_plus1_flag;
  v.seq_ref_layer_chroma_phase_y_plus1 = v.chroma_phase_y_plus1;
  if (v.extended_spatial_scalability_idc == 1) {
    if (chroma_array_type > 0) {
      v.seq_ref_layer_chroma_phase_x_plus1_flag = b.read_flag("seq_ref_layer_chroma_phase_x_plus1_flag");
      v.seq_ref_layer_chroma_phase_y_plus1 = static_cast<uint32_t>(b.read_u("seq_ref_layer_chroma_phase_y_plus1", 2));
      detail::check_range(b, "seq_ref_layer_chroma_phase_y_plus1", v.seq_ref_layer_chroma_phase_y_plus1, 0, 2,
                          "H.264 G.7.4.2.1.4");
    }
    const auto offset = [&b](std::string_view n) {
      const int32_t o = b.read_se(n);
      detail::check_range(b, n, o, -kMaxScaledOffset - 1, kMaxScaledOffset, "H.264 G.7.4.2.1.4");
      return o;
    };
    v.seq_scaled_ref_layer_left_offset = offset("seq_scaled_ref_layer_left_offset");
    v.seq_scaled_ref_layer_top_offset = offset("seq_scaled_ref_layer_top_offset");
    v.seq_scaled_ref_layer_right_offset = offset("seq_scaled_ref_layer_right_offset");
    v.seq_scaled_ref_layer_bottom_offset = offset("seq_scaled_ref_layer_bottom_offset");
  }
  v.seq_tcoeff_level_prediction_flag = b.read_flag("seq_tcoeff_level_prediction_flag");
  if (v.seq_tcoeff_level_prediction_flag) {
    v.adaptive_tcoeff_level_prediction_flag = b.read_flag("adaptive_tcoeff_level_prediction_flag");
  }
  v.slice_header_restriction_flag = b.read_flag("slice_header_restriction_flag");
}

// num_<name>[i] followed by <name>[i][j] for j < num_<name>[i].
std::vector<uint32_t> read_ref_list(BitReader& br, SyntaxBuilder& b, std::string_view count_name, std::string_view name,
                                    int i, uint32_t num_views_minus1) {
  const uint32_t n = detail::read_ue_at(b, count_name, i);
  detail::require_range(br, count_name, n, 0, std::min<int64_t>(kMaxInterViewRefs, num_views_minus1));
  std::vector<uint32_t> refs;
  for (uint32_t j = 0; j < n; ++j) {
    b.begin_array_element(name, i);
    refs.push_back(detail::read_ue_at(b, name, static_cast<int>(j)));
    b.end_array_element();
    detail::check_range(b, name, refs.back(), 0, kMaxViewIndex, "H.264 H.7.4.2.1.4");
  }
  return refs;
}

MvcApplicableOp read_applicable_op(BitReader& br, SyntaxBuilder& b, int i, int j, bool mvcd) {
  MvcApplicableOp op;
  b.begin_array_element("applicable_op", i);
  b.begin_array_element("applicable_op", j);
  op.temporal_id = static_cast<uint32_t>(b.read_u("applicable_op_temporal_id", 3));
  const uint32_t targets_minus1 = b.read_ue("applicable_op_num_target_views_minus1");
  detail::require_range(br, "applicable_op_num_target_views_minus1", targets_minus1, 0, kMaxViewIndex);
  for (uint32_t k = 0; k <= targets_minus1; ++k) {
    b.begin_array_element("applicable_op_target_view_id", static_cast<int>(k));
    op.target_view_id.push_back(b.read_ue("applicable_op_target_view_id"));
    detail::check_range(b, "applicable_op_target_view_id", op.target_view_id.back(), 0, kMaxViewIndex,
                        "H.264 H.7.4.2.1.4");
    if (mvcd) {
      op.depth_flag.push_back(b.read_flag("applicable_op_depth_flag"));
      op.texture_flag.push_back(b.read_flag("applicable_op_texture_flag"));
    }
    b.end_array_element();
  }
  if (mvcd) {
    // I.7.4.2.1.4: both 0..1023.
    op.num_texture_views_minus1 = b.read_ue("applicable_op_num_texture_views_minus1");
    detail::check_range(b, "applicable_op_num_texture_views_minus1", op.num_texture_views_minus1, 0, kMaxViewIndex,
                        "H.264 I.7.4.2.1.4");
    op.num_depth_views = b.read_ue("applicable_op_num_depth_views");
    detail::check_range(b, "applicable_op_num_depth_views", op.num_depth_views, 0, kMaxViewIndex, "H.264 I.7.4.2.1.4");
  } else {
    op.num_views_minus1 = b.read_ue("applicable_op_num_views_minus1");
    detail::check_range(b, "applicable_op_num_views_minus1", op.num_views_minus1, 0, kMaxViewIndex,
                        "H.264 H.7.4.2.1.4");
  }
  b.end_array_element();
  b.end_array_element();
  return op;
}

// The profile_idc 134 (MFC High) tail of seq_parameter_set_mvc_extension() (H.7.3.2.1.4).
void read_mfc_fields(SyntaxBuilder& b, const Sps& sps, MvcSpsExtension& m) {
  m.mfc_format_idc = static_cast<uint32_t>(b.read_u("mfc_format_idc", 6));
  if (m.mfc_format_idc > 1) {
    // H.7.4.2.1.4: 2..63 are reserved and a decoder ignores the coded video sequence.
    detail::warn_reserved(b, "mfc_format_idc", m.mfc_format_idc, "H.264 H.7.4.2.1.4");
  } else {
    m.default_grid_position_flag = b.read_flag("default_grid_position_flag");
    if (!m.default_grid_position_flag) {
      m.view0_grid_position_x = static_cast<uint32_t>(b.read_u("view0_grid_position_x", 4));
      m.view0_grid_position_y = static_cast<uint32_t>(b.read_u("view0_grid_position_y", 4));
      m.view1_grid_position_x = static_cast<uint32_t>(b.read_u("view1_grid_position_x", 4));
      m.view1_grid_position_y = static_cast<uint32_t>(b.read_u("view1_grid_position_y", 4));
    }
  }
  m.rpu_filter_enabled_flag = b.read_flag("rpu_filter_enabled_flag");
  if (!sps.frame_mbs_only_flag) {
    m.rpu_field_processing_flag = b.read_flag("rpu_field_processing_flag");
  }
}

// seq_parameter_set_mvc_extension() (H.7.3.2.1.4) or, with mvcd, seq_parameter_set_mvcd_extension() (I.7.3.2.1.5)
// up to its VUI flags. The MVCD extension adds the per-view depth / texture flags, reads the reference lists only for
// views with depth and replaces applicable_op_num_views_minus1 with texture and depth view counts.
void parse_mvc_extension(BitReader& br, SyntaxBuilder& b, const Sps& sps, bool mvcd, MvcSpsExtension& m) {
  m.num_views_minus1 = b.read_ue("num_views_minus1");
  detail::require_range(br, "num_views_minus1", m.num_views_minus1, 0, kMaxViewIndex);
  const uint32_t views = m.num_views_minus1 + 1;
  for (uint32_t i = 0; i < views; ++i) {
    b.begin_array_element("view_id", static_cast<int>(i));
    m.view_id.push_back(b.read_ue("view_id"));
    detail::check_range(b, "view_id", m.view_id.back(), 0, kMaxViewIndex, "H.264 H.7.4.2.1.4");
    if (mvcd) {
      m.depth_view_present_flag.push_back(b.read_flag("depth_view_present_flag"));
      if (m.depth_view_present_flag.back()) {
        ++m.NumDepthViews;
      }
      m.texture_view_present_flag.push_back(b.read_flag("texture_view_present_flag"));
    }
    b.end_array_element();
  }
  if (mvcd) {
    b.derived("NumDepthViews", static_cast<int64_t>(m.NumDepthViews));
  }
  const auto has_refs = [&m, mvcd](uint32_t i) { return !mvcd || m.depth_view_present_flag[i]; };
  m.anchor_ref_l0.resize(1);
  m.anchor_ref_l1.resize(1);
  for (uint32_t i = 1; i < views; ++i) {
    const auto idx = static_cast<int>(i);
    if (!has_refs(i)) {
      m.anchor_ref_l0.emplace_back();
      m.anchor_ref_l1.emplace_back();
      continue;
    }
    m.anchor_ref_l0.push_back(read_ref_list(br, b, "num_anchor_refs_l0", "anchor_ref_l0", idx, m.num_views_minus1));
    m.anchor_ref_l1.push_back(read_ref_list(br, b, "num_anchor_refs_l1", "anchor_ref_l1", idx, m.num_views_minus1));
  }
  m.non_anchor_ref_l0.resize(1);
  m.non_anchor_ref_l1.resize(1);
  for (uint32_t i = 1; i < views; ++i) {
    const auto idx = static_cast<int>(i);
    if (!has_refs(i)) {
      m.non_anchor_ref_l0.emplace_back();
      m.non_anchor_ref_l1.emplace_back();
      continue;
    }
    m.non_anchor_ref_l0.push_back(
        read_ref_list(br, b, "num_non_anchor_refs_l0", "non_anchor_ref_l0", idx, m.num_views_minus1));
    m.non_anchor_ref_l1.push_back(
        read_ref_list(br, b, "num_non_anchor_refs_l1", "non_anchor_ref_l1", idx, m.num_views_minus1));
  }
  const uint32_t levels_minus1 = b.read_ue("num_level_values_signalled_minus1");
  detail::require_range(br, "num_level_values_signalled_minus1", levels_minus1, 0, kMaxLevelValuesMinus1);
  for (uint32_t i = 0; i <= levels_minus1; ++i) {
    const auto idx = static_cast<int>(i);
    MvcLevelValue lv;
    b.begin_array_element("level_idc", idx);
    lv.level_idc = static_cast<uint32_t>(b.read_u("level_idc", 8));
    b.end_array_element();
    const uint32_t ops_minus1 = detail::read_ue_at(b, "num_applicable_ops_minus1", idx);
    detail::require_range(br, "num_applicable_ops_minus1", ops_minus1, 0, kMaxViewIndex);
    for (uint32_t j = 0; j <= ops_minus1; ++j) {
      lv.ops.push_back(read_applicable_op(br, b, idx, static_cast<int>(j), mvcd));
    }
    m.level_values.push_back(std::move(lv));
  }
  if (!mvcd && sps.profile_idc == 134) {
    read_mfc_fields(b, sps, m);
  }
}

// additional_extension2_flag; if set, while (more_rbsp_data()) additional_extension2_data_flag; rbsp_trailing_bits().
void read_extension2_and_trailing_bits(BitReader& br, SyntaxBuilder& b, SubsetSps& s) {
  s.additional_extension2_flag = b.read_flag("additional_extension2_flag");
  if (s.additional_extension2_flag) {
    while (br.more_rbsp_data()) {
      (void)b.read_flag("additional_extension2_data_flag");
      ++s.additional_extension2_data_bits;
    }
  }
  detail::read_rbsp_trailing_bits(br, b);
}

}  // namespace

SubsetSps parse_subset_sps(BitReader& br, SyntaxBuilder& b) {
  SubsetSps s;
  const auto start = br.mark();
  auto scope = b.scoped("subset_seq_parameter_set_rbsp", "H.264 7.3.2.1.3");
  s.sps = parse_seq_parameter_set_data(br, b);
  const uint32_t profile = s.sps.profile_idc;
  if (profile == 83 || profile == 86) {
    s.extension = SubsetSpsExtension::Svc;
    parse_svc_extension(b, s.sps.ChromaArrayType, s.svc);
    s.svc_vui_parameters_present_flag = b.read_flag("svc_vui_parameters_present_flag");
    if (s.svc_vui_parameters_present_flag) {
      s.svc_vui = parse_vui_extension(br, b, Ext::Svc);
    }
  } else if (profile == 118 || profile == 128 || profile == 134) {
    s.extension = SubsetSpsExtension::Mvc;
    s.bit_equal_to_one = b.read_f("bit_equal_to_one", 1, 1) == 1;
    {
      auto ext = b.scoped("seq_parameter_set_mvc_extension", "H.264 H.7.3.2.1.4");
      parse_mvc_extension(br, b, s.sps, false, s.mvc);
    }
    s.mvc_vui_parameters_present_flag = b.read_flag("mvc_vui_parameters_present_flag");
    if (s.mvc_vui_parameters_present_flag) {
      s.mvc_vui = parse_vui_extension(br, b, Ext::Mvc);
    }
  } else if (profile == 135 || profile == 138 || profile == 139) {
    s.extension = profile == 139 ? SubsetSpsExtension::Mvcd3dAvc : SubsetSpsExtension::Mvcd;
    s.bit_equal_to_one = b.read_f("bit_equal_to_one", 1, 1) == 1;
    {
      auto ext = b.scoped("seq_parameter_set_mvcd_extension", "H.264 I.7.3.2.1.5");
      parse_mvc_extension(br, b, s.sps, true, s.mvc);
      s.mvcd_vui_parameters_present_flag = b.read_flag("mvcd_vui_parameters_present_flag");
      if (s.mvcd_vui_parameters_present_flag) {
        s.mvcd_vui = parse_vui_extension(br, b, Ext::Mvcd);
      }
      s.texture_vui_parameters_present_flag = b.read_flag("texture_vui_parameters_present_flag");
      if (s.texture_vui_parameters_present_flag) {
        s.mvc_vui = parse_vui_extension(br, b, Ext::Mvc);
      }
    }
    if (profile == 139) {
      // SPEC-ASSUMPTION(#552): seq_parameter_set_3davc_extension() (J.7.3.2.1.5) is not parsed yet.
      detail::keep_as_bytes(b, "subset_sps_extension", "H.264 J.7.3.2.1.5");
      s.span = br.span_since(start);
      return s;
    }
  }
  read_extension2_and_trailing_bits(br, b, s);
  s.span = br.span_since(start);
  return s;
}

}  // namespace ovt::syntax::h264
