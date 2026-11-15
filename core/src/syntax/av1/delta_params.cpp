// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 delta_q_params() and delta_lf_params() (chapter 7 §7.3.6 rows 18 and 19; WI-1.117).

#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecDeltaQ = "AV1 5.9.17";
constexpr std::string_view kSpecDeltaLf = "AV1 5.9.18";

}  // namespace

DeltaQParams parse_delta_q_params(BitReader& br, SyntaxBuilder& b, uint8_t base_q_idx) {
  // AV1 §5.9.17
  DeltaQParams d;
  const auto start = br.mark();
  auto s = b.scoped("delta_q_params", kSpecDeltaQ);
  d.delta_q_res = 0;
  d.delta_q_present = false;
  if (base_q_idx > 0) {
    d.delta_q_present = b.read_flag("delta_q_present");
  } else {
    b.derived("delta_q_present", "delta_q_present = 0 (base_q_idx = 0)");
  }
  if (d.delta_q_present) {
    d.delta_q_res = static_cast<uint8_t>(b.read_u("delta_q_res", 2));
  }
  d.span = br.span_since(start);
  return d;
}

DeltaLfParams parse_delta_lf_params(BitReader& br, SyntaxBuilder& b, bool delta_q_present, bool allow_intrabc) {
  // AV1 §5.9.18
  DeltaLfParams d;
  const auto start = br.mark();
  auto s = b.scoped("delta_lf_params", kSpecDeltaLf);
  d.delta_lf_present = false;
  d.delta_lf_res = 0;
  d.delta_lf_multi = false;
  if (delta_q_present) {
    if (!allow_intrabc) {
      d.delta_lf_present = b.read_flag("delta_lf_present");
    } else {
      b.derived("delta_lf_present", "delta_lf_present = 0 (allow_intrabc)");
    }
    if (d.delta_lf_present) {
      d.delta_lf_res = static_cast<uint8_t>(b.read_u("delta_lf_res", 2));
      d.delta_lf_multi = b.read_flag("delta_lf_multi");
    }
  }
  d.span = br.span_since(start);
  return d;
}

}  // namespace ovt::syntax::av1
