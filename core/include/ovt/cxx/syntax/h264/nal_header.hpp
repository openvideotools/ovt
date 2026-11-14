// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 nal_unit() header (§7.3.1) with the SVC (G.7.3.1.1), MVC (H.7.3.1.1) and 3D-AVC (I.7.3.1.1)
// extension headers: chapter 7 §7.3.4 row 1, WI-1.108. C++ helper header (D22): semi-public, no ABI promise.
//
// The parser reads from a BitReader positioned at the first byte of the RBSP (the NAL header is part of the
// RBSP that rbsp_from_nal() returns), so bit offsets in the tree equal trace_headers' offsets.

#ifndef OVT_CXX_SYNTAX_H264_NAL_HEADER_HPP
#define OVT_CXX_SYNTAX_H264_NAL_HEADER_HPP

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264 {

/// nal_unit_header_svc_extension() (G.7.3.1.1).
struct NalHeaderSvcExtension {
  bool idr_flag = false;
  uint32_t priority_id = 0;
  bool no_inter_layer_pred_flag = false;
  uint32_t dependency_id = 0;
  uint32_t quality_id = 0;
  uint32_t temporal_id = 0;
  bool use_ref_base_pic_flag = false;
  bool discardable_flag = false;
  bool output_flag = false;
  uint32_t reserved_three_2bits = 3;
};

/// nal_unit_header_mvc_extension() (H.7.3.1.1).
struct NalHeaderMvcExtension {
  bool non_idr_flag = false;
  uint32_t priority_id = 0;
  uint32_t view_id = 0;
  uint32_t temporal_id = 0;
  bool anchor_pic_flag = false;
  bool inter_view_flag = false;
  uint32_t reserved_one_bit = 1;
};

/// nal_unit_header_3davc_extension() (I.7.3.1.1).
struct NalHeader3dAvcExtension {
  uint32_t view_idx = 0;
  bool depth_flag = false;
  bool non_idr_flag = false;
  uint32_t temporal_id = 0;
  bool anchor_pic_flag = false;
  bool inter_view_flag = false;
};

struct NalHeader {
  // syntax elements
  uint32_t forbidden_zero_bit = 0;
  uint32_t nal_ref_idc = 0;
  uint32_t nal_unit_type = 0;
  bool svc_extension_flag = false;     // types 14, 20
  bool avc_3d_extension_flag = false;  // type 21
  bool has_svc_extension = false;
  bool has_mvc_extension = false;
  bool has_3davc_extension = false;
  NalHeaderSvcExtension svc;
  NalHeaderMvcExtension mvc;
  NalHeader3dAvcExtension avc_3d;
  // derived (§7.4.1)
  bool IdrPicFlag = false;     // nal_unit_type == 5
  std::string_view type_name;  // Table 7-1 (static storage)
  uint32_t temporal_id = 0;    // Unit.temporal_id: SVC/MVC/3D-AVC temporal_id, else 0
  uint32_t layer_id = 0;       // Unit.layer_id: dependency_id (SVC), view_id (MVC), view_idx (3D-AVC), else 0
  bool reserved_type = false;  // Table 7-1 reserved types 17, 18, 22, 23 (Diagnostic SYN-H264-0001 raised)
  BitSpan span{0, 0};
};

/// Table 7-1 name of a nal_unit_type (0..31); "unspecified" / "reserved" outside the specified rows.
[[nodiscard]] std::string_view nal_unit_type_name(uint32_t nal_unit_type) noexcept;

/// Parses nal_unit() up to and including the extension header. Node `nal_unit_header`. Never throws for
/// forbidden or reserved values (they become Diagnostic Error SYN-H264-0001); only reader errors propagate.
NalHeader parse_nal_header(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_NAL_HEADER_HPP
