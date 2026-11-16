// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 prefix_nal_unit_rbsp() (§7.3.2.12) with prefix_nal_unit_svc() (G.7.3.2.12.1) and dec_ref_base_pic_marking()
// (G.7.3.3.5): chapter 7 §7.3.4 row 23 and §7.3.7, WI-1.109. Clause letters follow H.264 (08/2021). C++ helper header
// (D22): semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H264_PREFIX_NAL_HPP
#define OVT_CXX_SYNTAX_H264_PREFIX_NAL_HPP

#include <cstdint>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {
class SyntaxNodeBuffer;
}

namespace ovt::syntax::h264 {

struct NalHeader;

/// One memory_management_base_control_operation of dec_ref_base_pic_marking() (G.7.3.3.5).
struct MmcoBaseOp {
  uint32_t memory_management_base_control_operation = 0;  // Table G-3: 1 short-term unused, 2 long-term unused
  uint32_t difference_of_base_pic_nums_minus1 = 0;
  uint32_t long_term_base_pic_num = 0;
};

/// dec_ref_base_pic_marking() (G.7.3.3.5). The terminating operation 0 is not stored.
struct DecRefBasePicMarking {
  bool adaptive_ref_base_pic_marking_mode_flag = false;
  std::vector<MmcoBaseOp> ops;
};

/// prefix_nal_unit_svc() (G.7.3.2.12.1).
struct PrefixNalUnitSvc {
  bool store_ref_base_pic_flag = false;
  bool has_dec_ref_base_pic_marking = false;
  DecRefBasePicMarking dec_ref_base_pic_marking;
  bool additional_prefix_nal_unit_extension_flag = false;
  uint64_t additional_prefix_nal_unit_extension_data_bits = 0;  // derived: number of ..._data_flag read
};

/// prefix_nal_unit_rbsp(), row 23 (nal_unit_type 14). Only an SVC prefix (svc_extension_flag = 1) has a payload,
/// prefix_nal_unit_svc() (G.7.3.2.12.1); an MVC prefix is empty.
struct PrefixNalUnit {
  bool svc = false;  // svc_extension_flag of the NAL header
  PrefixNalUnitSvc prefix_svc;
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 23. Node `prefix_nal_unit_rbsp`. `nal` is the unit's parsed NAL header: svc_extension_flag selects the
/// payload, and nal_ref_idc, idr_flag and use_ref_base_pic_flag drive prefix_nal_unit_svc(). Throws ParseError
/// SYN-H264-0002 on a memory_management_base_control_operation above 2.
PrefixNalUnit parse_prefix_nal_unit(BitReader& br, SyntaxBuilder& b, const NalHeader& nal);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_PREFIX_NAL_HPP
