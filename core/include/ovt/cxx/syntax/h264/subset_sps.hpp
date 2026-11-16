// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 subset_seq_parameter_set_rbsp() (§7.3.2.1.3) with the MVC extension (H.7.3.2.1.4): chapter 7 §7.3.4 row 22 and
// §7.3.7, WI-1.109. Headers only: OVT parses these so that SVC / MVC streams show a complete tree; block analysis of
// enhancement layers is out of scope. C++ helper header (D22): semi-public, no ABI promise.

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

/// One operation point j of level value i in seq_parameter_set_mvc_extension(): applicable_op_*[i][j].
struct MvcApplicableOp {
  uint32_t temporal_id = 0;
  std::vector<uint32_t> target_view_id;  // applicable_op_target_view_id[i][j][k]
  uint32_t num_views_minus1 = 0;
};

/// One level value i of seq_parameter_set_mvc_extension().
struct MvcLevelValue {
  uint32_t level_idc = 0;
  std::vector<MvcApplicableOp> ops;
};

/// seq_parameter_set_mvc_extension() (H.7.3.2.1.4). The per-view lists are indexed by view order index i; entry 0
/// (the base view) is empty.
struct MvcSpsExtension {
  uint32_t num_views_minus1 = 0;
  std::vector<uint32_t> view_id;
  std::vector<std::vector<uint32_t>> anchor_ref_l0;
  std::vector<std::vector<uint32_t>> anchor_ref_l1;
  std::vector<std::vector<uint32_t>> non_anchor_ref_l0;
  std::vector<std::vector<uint32_t>> non_anchor_ref_l1;
  std::vector<MvcLevelValue> level_values;
};

/// Which extension follows seq_parameter_set_data() in a subset SPS (selected by profile_idc, §7.3.2.1.3).
enum class SubsetSpsExtension {
  None,         // a profile_idc without an extension in this syntax structure
  Mvc,          // 118, 128, 134: seq_parameter_set_mvc_extension()
  KeptAsBytes,  // 83, 86 (SVC) and 135, 138, 139 (MVCD / 3D-AVC): kept as bytes, see parse_subset_sps()
};

/// subset_seq_parameter_set_rbsp(), row 22 (nal_unit_type 15).
struct SubsetSps {
  Sps sps;  // seq_parameter_set_data(): what ParameterSets::subset_sps holds (H.7.4.1.2.1)
  SubsetSpsExtension extension = SubsetSpsExtension::None;
  bool bit_equal_to_one = true;
  MvcSpsExtension mvc;
  bool mvc_vui_parameters_present_flag = false;
  bool additional_extension2_flag = false;
  uint64_t additional_extension2_data_bits = 0;  // derived: number of additional_extension2_data_flag read
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 22. Node `subset_seq_parameter_set_rbsp`. The reader stands after the NAL header. Throws ParseError
/// SYN-H264-0002 on a loop bound out of range (§7.3.1 rule 3).
// SPEC-ASSUMPTION(#552): the SVC (83, 86) and MVCD / 3D-AVC (135, 138, 139) extensions and
// mvc_vui_parameters_extension() are parsed by WI-1.109d; until then the rest of the RBSP is kept as the opaque node
// `subset_sps_extension` with Info SYN-H264-0071.
SubsetSps parse_subset_sps(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_SUBSET_SPS_HPP
