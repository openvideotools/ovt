// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Helpers shared by the H.265 header parsers (chapter 7 §7.3.5, WI-1.112): array-element reads, range checks
// that raise SYN-H265-0007, rbsp_trailing_bits() and Ceil(Log2()). Module-internal, not installed.

#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265::detail {

/// Reads one u(1) element `name[idx]` so that the node carries array_index = idx.
inline bool read_flag_at(SyntaxBuilder& b, std::string_view name, int idx) {
  b.begin_array_element(name, idx);
  const bool v = b.read_flag(name);
  b.end_array_element();
  return v;
}

/// Reads one ue(v) element `name[idx]`.
inline uint32_t read_ue_at(SyntaxBuilder& b, std::string_view name, int idx) {
  b.begin_array_element(name, idx);
  const uint32_t v = b.read_ue(name);
  b.end_array_element();
  return v;
}

/// Reads one se(v) element `name[idx]`.
inline int32_t read_se_at(SyntaxBuilder& b, std::string_view name, int idx) {
  b.begin_array_element(name, idx);
  const int32_t v = b.read_se(name);
  b.end_array_element();
  return v;
}

/// Reads one u(n) element `name[idx]`; `descriptor` overrides the descriptor text ("u(v)") when non-empty.
inline uint64_t read_u_at(SyntaxBuilder& b, std::string_view name, int idx, unsigned n,
                          std::string_view descriptor = {}) {
  b.begin_array_element(name, idx);
  const uint64_t v = b.read_u(name, n);
  if (!descriptor.empty()) {
    b.set_descriptor_text(descriptor);
  }
  b.end_array_element();
  return v;
}

/// Diagnostic Error SYN-H265-0007 on the last node when `value` is outside [lo, hi]; parsing continues. Returns
/// true when the value is in range.
inline bool check_range(SyntaxBuilder& b, std::string_view name, int64_t value, int64_t lo, int64_t hi,
                        std::string_view spec_ref = {}) {
  if (value >= lo && value <= hi) {
    return true;
  }
  b.diag(Severity::Error, "SYN-H265-0007",
         fmt::format("{} out of range ({} = {}, allowed [{},{}])", name, name, value, lo, hi), spec_ref, true);
  return false;
}

/// ParseError SYN-H265-0007 when `value` is outside [lo, hi]: used for elements that size later loops, arrays or
/// bit widths, where continuing would read garbage.
inline void require_range(const BitReader& br, std::string_view name, int64_t value, int64_t lo, int64_t hi) {
  if (value < lo || value > hi) {
    throw ParseError{"SYN-H265-0007",
                     fmt::format("{} out of range ({} = {}, allowed [{},{}])", name, name, value, lo, hi),
                     br.bit_pos()};
  }
}

/// H.265 §7.3.2.11 rbsp_trailing_bits(): rbsp_stop_one_bit then rbsp_alignment_zero_bit up to the byte boundary,
/// recorded as nodes. A mismatch is Diagnostic Error SYN-GEN-0001 (read_f), not a ParseError.
inline void read_rbsp_trailing_bits(BitReader& br, SyntaxBuilder& b) {
  auto s = b.scoped("rbsp_trailing_bits", "H.265 7.3.2.11");
  (void)b.read_f("rbsp_stop_one_bit", 1, 1);
  while (!br.byte_aligned()) {
    (void)b.read_f("rbsp_alignment_zero_bit", 1, 0);
  }
}

/// Ceil(Log2(v)) for v >= 1 (H.265 §5.8); 0 for v <= 1.
constexpr unsigned ceil_log2(uint64_t v) noexcept {
  unsigned n = 0;
  while ((uint64_t{1} << n) < v) {
    ++n;
  }
  return n;
}

}  // namespace ovt::syntax::h265::detail
