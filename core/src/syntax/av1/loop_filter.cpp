// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 loop_filter_params() (chapter 7 §7.3.6 row 20; WI-1.118).

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecLoopFilter = "AV1 5.9.11";

// The update_ref_delta / update_mode_delta loops of AV1 §5.9.11 (loop_filter_delta_update == 1).
void read_delta_updates(SyntaxBuilder& b, LoopFilterParams& lf) {
  for (int i = 0; i < kTotalRefsPerFrame; ++i) {
    const auto si = static_cast<size_t>(i);
    b.begin_array_element("loop_filter_ref_delta", i, kSpecLoopFilter);
    lf.update_ref_delta[si] = b.read_flag("update_ref_delta");
    b.set_enum(ref_frame_name(i));
    if (lf.update_ref_delta[si]) {
      lf.loop_filter_ref_deltas[si] = static_cast<int8_t>(b.read_su("loop_filter_ref_deltas", 7));
      b.set_descriptor_text("su(1+6)");
    }
    b.end_array_element();
  }
  for (int i = 0; i < 2; ++i) {
    const auto si = static_cast<size_t>(i);
    b.begin_array_element("loop_filter_mode_delta", i, kSpecLoopFilter);
    lf.update_mode_delta[si] = b.read_flag("update_mode_delta");
    if (lf.update_mode_delta[si]) {
      lf.loop_filter_mode_deltas[si] = static_cast<int8_t>(b.read_su("loop_filter_mode_deltas", 7));
      b.set_descriptor_text("su(1+6)");
    }
    b.end_array_element();
  }
}

}  // namespace

LoopFilterParams parse_loop_filter_params(BitReader& br, SyntaxBuilder& b, const ColorConfig& cc, bool CodedLossless,
                                          bool allow_intrabc,
                                          const std::array<int8_t, kTotalRefsPerFrame>& prev_ref_deltas,
                                          const std::array<int8_t, 2>& prev_mode_deltas) {
  // AV1 §5.9.11
  LoopFilterParams lf;
  const auto start = br.mark();
  auto s = b.scoped("loop_filter_params", kSpecLoopFilter);
  if (CodedLossless || allow_intrabc) {
    lf.loop_filter_level[0] = 0;
    lf.loop_filter_level[1] = 0;
    lf.loop_filter_ref_deltas = kDefaultLoopFilterRefDeltas;
    lf.loop_filter_mode_deltas = {0, 0};
    b.derived("loop_filter_params", CodedLossless ? "loop_filter_level[0..1] = 0, default deltas (CodedLossless)"
                                                  : "loop_filter_level[0..1] = 0, default deltas (allow_intrabc)");
    lf.span = br.span_since(start);
    return lf;
  }
  lf.coded = true;
  lf.loop_filter_ref_deltas = prev_ref_deltas;
  lf.loop_filter_mode_deltas = prev_mode_deltas;
  for (int i = 0; i < 2; ++i) {
    b.begin_array_element("loop_filter_level", i, kSpecLoopFilter);
    lf.loop_filter_level[static_cast<size_t>(i)] = static_cast<uint8_t>(b.read_u("loop_filter_level", 6));
    b.end_array_element();
  }
  if (cc.NumPlanes > 1 && (lf.loop_filter_level[0] != 0 || lf.loop_filter_level[1] != 0)) {
    for (int i = 2; i < 4; ++i) {
      b.begin_array_element("loop_filter_level", i, kSpecLoopFilter);
      lf.loop_filter_level[static_cast<size_t>(i)] = static_cast<uint8_t>(b.read_u("loop_filter_level", 6));
      b.end_array_element();
    }
  }
  lf.loop_filter_sharpness = static_cast<uint8_t>(b.read_u("loop_filter_sharpness", 3));
  lf.loop_filter_delta_enabled = b.read_flag("loop_filter_delta_enabled");
  if (lf.loop_filter_delta_enabled) {
    lf.loop_filter_delta_update = b.read_flag("loop_filter_delta_update");
    if (lf.loop_filter_delta_update) {
      read_delta_updates(b, lf);
    }
  }
  const auto& r = lf.loop_filter_ref_deltas;
  b.derived(
      "loop_filter_deltas",
      fmt::format("loop_filter_ref_deltas = {{{}, {}, {}, {}, {}, {}, {}, {}}}, loop_filter_mode_deltas = {{{}, {}}}",
                  r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], lf.loop_filter_mode_deltas[0],
                  lf.loop_filter_mode_deltas[1]));
  lf.span = br.span_since(start);
  return lf;
}

}  // namespace ovt::syntax::av1
