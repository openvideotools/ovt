// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 seq_parameter_set_extension_rbsp() (§7.3.2.1.2): chapter 7 §7.3.4 row 21 and §7.3.7, WI-1.109. C++ helper
// header (D22): semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H264_SPS_EXT_HPP
#define OVT_CXX_SYNTAX_H264_SPS_EXT_HPP

#include <cstdint>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {
class SyntaxNodeBuffer;
}

namespace ovt::syntax::h264 {

/// seq_parameter_set_extension_rbsp(), row 21 (nal_unit_type 13): the auxiliary (alpha) picture format.
struct SpsExtension {
  uint32_t seq_parameter_set_id = 0;
  uint32_t aux_format_idc = 0;
  uint32_t bit_depth_aux_minus8 = 0;
  bool alpha_incr_flag = false;
  uint32_t alpha_opaque_value = 0;
  uint32_t alpha_transparent_value = 0;
  bool additional_extension_flag = false;
  // derived
  uint32_t BitDepthAux = 0;  // bit_depth_aux_minus8 + 8 when aux_format_idc != 0, else 0
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 21. Node `seq_parameter_set_extension_rbsp`. The reader stands after the NAL header. Throws ParseError
/// SYN-H264-0002 when seq_parameter_set_id > 31, aux_format_idc > 3 or bit_depth_aux_minus8 > 4 (§7.4.2.1.2).
SpsExtension parse_sps_extension(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_SPS_EXT_HPP
