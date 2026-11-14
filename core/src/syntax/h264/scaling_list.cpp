// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 scaling_list() (§7.3.2.1.1.1) with useDefaultScalingMatrixFlag (chapter 7 §7.3.4 row 3, WI-1.108).
// The Table 7-2 fall-back rules are applied at PPS activation (§7.3.8, WI-1.111), not here.

#include <cstdint>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

void parse_scaling_list(BitReader& br, SyntaxBuilder& b, ScalingList& out, uint32_t size, int index) {
  out.present = true;
  out.size = size;
  out.UseDefaultScalingMatrixFlag = false;
  const auto start = br.mark();
  b.begin_array_element("scaling_list", index, "H.264 7.3.2.1.1.1");
  int32_t last_scale = 8;
  int32_t next_scale = 8;
  for (uint32_t j = 0; j < size; ++j) {
    if (next_scale != 0) {
      b.begin_array_element("delta_scale", static_cast<int>(j));
      const int32_t delta_scale = b.read_se("delta_scale");
      b.end_array_element();
      // SPEC-ASSUMPTION(#388): §7.4.2.1.1.1 bounds delta_scale to -128..127; a value outside it is a ParseError
      // (§7.3.1 rule 3) so that the nextScale arithmetic below cannot overflow.
      detail::require_range(br, "delta_scale", delta_scale, -128, 127);
      out.delta_scale.at(j) = delta_scale;
      next_scale = (last_scale + delta_scale + 256) % 256;
      out.UseDefaultScalingMatrixFlag = (j == 0 && next_scale == 0);
    }
    const int32_t v = (next_scale == 0) ? last_scale : next_scale;
    out.list.at(j) = static_cast<uint8_t>(v);
    last_scale = v;
  }
  b.derived("UseDefaultScalingMatrixFlag", out.UseDefaultScalingMatrixFlag ? 1 : 0);
  b.end_array_element();
  out.span = br.span_since(start);
}

}  // namespace ovt::syntax::h264
