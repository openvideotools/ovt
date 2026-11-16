// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 seq_parameter_set_extension_rbsp() (chapter 7 §7.3.4 row 21, WI-1.109). Element order follows H.264 (08/2021)
// §7.3.2.1.2 line by line; ranges from §7.4.2.1.2.

#include "ovt/cxx/syntax/h264/sps_ext.hpp"

#include <cstdint>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

SpsExtension parse_sps_extension(BitReader& br, SyntaxBuilder& b) {
  SpsExtension e;
  const auto start = br.mark();
  auto scope = b.scoped("seq_parameter_set_extension_rbsp", "H.264 7.3.2.1.2");
  e.seq_parameter_set_id = b.read_ue("seq_parameter_set_id");
  detail::require_range(br, "seq_parameter_set_id", e.seq_parameter_set_id, 0, 31);
  e.aux_format_idc = b.read_ue("aux_format_idc");
  detail::require_range(br, "aux_format_idc", e.aux_format_idc, 0, 3);
  if (e.aux_format_idc != 0) {
    e.bit_depth_aux_minus8 = b.read_ue("bit_depth_aux_minus8");
    detail::require_range(br, "bit_depth_aux_minus8", e.bit_depth_aux_minus8, 0, 4);
    e.BitDepthAux = e.bit_depth_aux_minus8 + 8;
    b.derived("BitDepthAux", e.BitDepthAux);
    e.alpha_incr_flag = b.read_flag("alpha_incr_flag");
    const unsigned v = e.bit_depth_aux_minus8 + 9;
    e.alpha_opaque_value = static_cast<uint32_t>(b.read_u("alpha_opaque_value", v));
    b.set_descriptor_text("u(v)");
    e.alpha_transparent_value = static_cast<uint32_t>(b.read_u("alpha_transparent_value", v));
    b.set_descriptor_text("u(v)");
  }
  e.additional_extension_flag = b.read_flag("additional_extension_flag");
  if (e.additional_extension_flag) {
    // §7.4.2.1.2: 1 is reserved; decoders ignore the flag (and anything after it in this unit).
    detail::warn_reserved(b, "additional_extension_flag", 1, "H.264 7.4.2.1.2");
    b.opaque_rest("additional_extension_data");
  } else {
    detail::read_rbsp_trailing_bits(br, b);
  }
  e.span = br.span_since(start);
  return e;
}

}  // namespace ovt::syntax::h264
