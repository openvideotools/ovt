// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 prefix_nal_unit_rbsp() (§7.3.2.12): chapter 7 §7.3.4 row 23 and §7.3.7, WI-1.109. C++ helper header (D22):
// semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H264_PREFIX_NAL_HPP
#define OVT_CXX_SYNTAX_H264_PREFIX_NAL_HPP

#include <cstdint>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {
class SyntaxNodeBuffer;
}

namespace ovt::syntax::h264 {

struct NalHeader;

/// prefix_nal_unit_rbsp(), row 23 (nal_unit_type 14). Only an SVC prefix (svc_extension_flag = 1) has a payload,
/// prefix_nal_unit_svc() (G.7.3.2.12.1); an MVC prefix is empty.
// SPEC-ASSUMPTION(#552): prefix_nal_unit_svc() is parsed by WI-1.109d; until then its bytes are kept as the opaque
// node `prefix_nal_unit_svc` with Info SYN-H264-0071.
struct PrefixNalUnit {
  bool svc = false;                // svc_extension_flag of the NAL header
  bool svc_kept_as_bytes = false;  // an SVC prefix with a payload
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 23. Node `prefix_nal_unit_rbsp`. `nal` is the unit's parsed NAL header (svc_extension_flag selects the
/// payload).
PrefixNalUnit parse_prefix_nal_unit(BitReader& br, SyntaxBuilder& b, const NalHeader& nal);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_PREFIX_NAL_HPP
