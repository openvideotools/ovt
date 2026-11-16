// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 prefix_nal_unit_rbsp() (chapter 7 §7.3.4 row 23, WI-1.109): §7.3.2.12.

#include "ovt/cxx/syntax/h264/prefix_nal.hpp"

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

PrefixNalUnit parse_prefix_nal_unit(BitReader& br, SyntaxBuilder& b, const NalHeader& nal) {
  PrefixNalUnit p;
  p.svc = nal.svc_extension_flag;
  const auto start = br.mark();
  auto scope = b.scoped("prefix_nal_unit_rbsp", "H.264 7.3.2.12");
  // if (svc_extension_flag) prefix_nal_unit_svc(). An SVC prefix of a non-reference picture may be empty.
  if (p.svc && br.bits_left() > 0) {
    p.svc_kept_as_bytes = true;
    detail::keep_as_bytes(b, "prefix_nal_unit_svc", "H.264 G.7.3.2.12.1");
  }
  p.span = br.span_since(start);
  return p;
}

}  // namespace ovt::syntax::h264
