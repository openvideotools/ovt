// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 subset_seq_parameter_set_rbsp() (chapter 7 §7.3.4 row 22, WI-1.109). The outer structure follows row 22
// (H.264 (08/2021) §7.3.2.1.3); seq_parameter_set_mvc_extension() follows H.7.3.2.1.4 and is checked against
// GStreamer gst_h264_parser_parse_subset_sps() (acceptance test 3).

#include "ovt/cxx/syntax/h264/subset_sps.hpp"

#include <algorithm>
#include <cstdint>
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
// 0..63 and each reference list holds at most Min(15, num_views_minus1) views.
constexpr int64_t kMaxViewIndex = 1023;
constexpr int64_t kMaxLevelValuesMinus1 = 63;
constexpr int64_t kMaxInterViewRefs = 15;

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

MvcApplicableOp read_applicable_op(BitReader& br, SyntaxBuilder& b, int i, int j) {
  MvcApplicableOp op;
  b.begin_array_element("applicable_op", i);
  b.begin_array_element("applicable_op", j);
  op.temporal_id = static_cast<uint32_t>(b.read_u("applicable_op_temporal_id", 3));
  const uint32_t targets_minus1 = b.read_ue("applicable_op_num_target_views_minus1");
  detail::require_range(br, "applicable_op_num_target_views_minus1", targets_minus1, 0, kMaxViewIndex);
  for (uint32_t k = 0; k <= targets_minus1; ++k) {
    op.target_view_id.push_back(detail::read_ue_at(b, "applicable_op_target_view_id", static_cast<int>(k)));
    detail::check_range(b, "applicable_op_target_view_id", op.target_view_id.back(), 0, kMaxViewIndex,
                        "H.264 H.7.4.2.1.4");
  }
  op.num_views_minus1 = b.read_ue("applicable_op_num_views_minus1");
  detail::check_range(b, "applicable_op_num_views_minus1", op.num_views_minus1, 0, kMaxViewIndex, "H.264 H.7.4.2.1.4");
  b.end_array_element();
  b.end_array_element();
  return op;
}

void parse_mvc_extension(BitReader& br, SyntaxBuilder& b, MvcSpsExtension& m) {
  auto scope = b.scoped("seq_parameter_set_mvc_extension", "H.264 H.7.3.2.1.4");
  m.num_views_minus1 = b.read_ue("num_views_minus1");
  detail::require_range(br, "num_views_minus1", m.num_views_minus1, 0, kMaxViewIndex);
  const uint32_t views = m.num_views_minus1 + 1;
  for (uint32_t i = 0; i < views; ++i) {
    m.view_id.push_back(detail::read_ue_at(b, "view_id", static_cast<int>(i)));
    detail::check_range(b, "view_id", m.view_id.back(), 0, kMaxViewIndex, "H.264 H.7.4.2.1.4");
  }
  m.anchor_ref_l0.resize(1);
  m.anchor_ref_l1.resize(1);
  for (uint32_t i = 1; i < views; ++i) {
    const auto idx = static_cast<int>(i);
    m.anchor_ref_l0.push_back(read_ref_list(br, b, "num_anchor_refs_l0", "anchor_ref_l0", idx, m.num_views_minus1));
    m.anchor_ref_l1.push_back(read_ref_list(br, b, "num_anchor_refs_l1", "anchor_ref_l1", idx, m.num_views_minus1));
  }
  m.non_anchor_ref_l0.resize(1);
  m.non_anchor_ref_l1.resize(1);
  for (uint32_t i = 1; i < views; ++i) {
    const auto idx = static_cast<int>(i);
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
      lv.ops.push_back(read_applicable_op(br, b, idx, static_cast<int>(j)));
    }
    m.level_values.push_back(std::move(lv));
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
  if (profile == 118 || profile == 128 || profile == 134) {
    s.extension = SubsetSpsExtension::Mvc;
    s.bit_equal_to_one = b.read_f("bit_equal_to_one", 1, 1) == 1;
    parse_mvc_extension(br, b, s.mvc);
    s.mvc_vui_parameters_present_flag = b.read_flag("mvc_vui_parameters_present_flag");
    if (s.mvc_vui_parameters_present_flag) {
      // SPEC-ASSUMPTION(#552): mvc_vui_parameters_extension() (H.14.1) is WI-1.109d's.
      detail::keep_as_bytes(b, "subset_sps_extension", "H.264 H.14.1");
      s.span = br.span_since(start);
      return s;
    }
  } else if (profile == 83 || profile == 86 || profile == 135 || profile == 138 || profile == 139) {
    // SPEC-ASSUMPTION(#552): the SVC (G.7.3.2.1.4) and MVCD / 3D-AVC (I.7.3.2.1.5) extensions are WI-1.109d's.
    s.extension = SubsetSpsExtension::KeptAsBytes;
    detail::keep_as_bytes(b, "subset_sps_extension",
                          profile == 83 || profile == 86 ? "H.264 G.7.3.2.1.4" : "H.264 I.7.3.2.1.5");
    s.span = br.span_since(start);
    return s;
  }
  read_extension2_and_trailing_bits(br, b, s);
  s.span = br.span_since(start);
  return s;
}

}  // namespace ovt::syntax::h264
