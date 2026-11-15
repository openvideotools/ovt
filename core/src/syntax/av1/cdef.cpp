// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 cdef_params() (chapter 7 §7.3.6 row 21; WI-1.118).

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecCdef = "AV1 5.9.19";

// cdef_*_sec_strength f(2): the value 3 stands for 4 (AV1 §5.9.19).
uint8_t read_sec_strength(SyntaxBuilder& b, std::string_view name) {
  auto v = static_cast<uint8_t>(b.read_u(name, 2));
  if (v == 3) {
    v = 4;
    b.derived(name, "3 → 4");
  }
  return v;
}

}  // namespace

CdefParams parse_cdef_params(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, bool CodedLossless,
                             bool allow_intrabc) {
  // AV1 §5.9.19
  CdefParams c;
  const auto start = br.mark();
  auto s = b.scoped("cdef_params", kSpecCdef);
  if (CodedLossless || allow_intrabc || !seq.enable_cdef) {
    c.cdef_bits = 0;
    c.cdef_y_pri_strength[0] = 0;
    c.cdef_y_sec_strength[0] = 0;
    c.cdef_uv_pri_strength[0] = 0;
    c.cdef_uv_sec_strength[0] = 0;
    c.CdefDamping = 3;
    b.derived("cdef_params", CodedLossless   ? "cdef_bits = 0, strengths 0, CdefDamping = 3 (CodedLossless)"
                             : allow_intrabc ? "cdef_bits = 0, strengths 0, CdefDamping = 3 (allow_intrabc)"
                                             : "cdef_bits = 0, strengths 0, CdefDamping = 3 (enable_cdef = 0)");
    c.span = br.span_since(start);
    return c;
  }
  c.coded = true;
  c.cdef_damping_minus_3 = static_cast<uint8_t>(b.read_u("cdef_damping_minus_3", 2));
  c.CdefDamping = static_cast<uint8_t>(c.cdef_damping_minus_3 + 3U);
  b.derived("CdefDamping", c.CdefDamping);
  c.cdef_bits = static_cast<uint8_t>(b.read_u("cdef_bits", 2));
  for (int i = 0; i < (1 << c.cdef_bits); ++i) {
    const auto si = static_cast<size_t>(i);
    b.begin_array_element("cdef_strength", i, kSpecCdef);
    c.cdef_y_pri_strength[si] = static_cast<uint8_t>(b.read_u("cdef_y_pri_strength", 4));
    c.cdef_y_sec_strength[si] = read_sec_strength(b, "cdef_y_sec_strength");
    if (seq.color_config.NumPlanes > 1) {
      c.cdef_uv_pri_strength[si] = static_cast<uint8_t>(b.read_u("cdef_uv_pri_strength", 4));
      c.cdef_uv_sec_strength[si] = read_sec_strength(b, "cdef_uv_sec_strength");
    }
    b.end_array_element();
  }
  c.span = br.span_since(start);
  return c;
}

}  // namespace ovt::syntax::av1
