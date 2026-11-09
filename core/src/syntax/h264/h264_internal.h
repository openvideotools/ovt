// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Helpers shared by the H.264 header parsers (chapter 7 §7.3.4): range checks, reserved-value warnings and
// rbsp_trailing_bits(). Module-internal.

#ifndef OVT_SRC_SYNTAX_H264_H264_INTERNAL_H
#define OVT_SRC_SYNTAX_H264_H264_INTERNAL_H

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264::detail {

// SPEC-ASSUMPTION(#320): generic H.264 range-violation ParseError code, mirroring SYN-H265-0007.
inline constexpr std::string_view kRangeCode = "SYN-H264-0002";
// SPEC-ASSUMPTION(#320): Warning for an unknown or reserved value that a decoder may ignore (§7.3.1 rule 3).
inline constexpr std::string_view kReservedCode = "SYN-H264-0003";

/// Throws ParseError SYN-H264-0002 unless lo <= value <= hi (§7.3.1 rule 3: checked before any loop is entered).
template <class T>
void require_range(const BitReader& br, std::string_view name, T value, int64_t lo, int64_t hi) {
  const auto v = static_cast<int64_t>(value);
  if (v < lo || v > hi) {
    throw ParseError{std::string(kRangeCode),
                     fmt::format("{} out of range ({} = {}, allowed [{},{}])", name, name, v, lo, hi), br.bit_pos()};
  }
}

/// Diagnostic Warning SYN-H264-0003 on the last emitted node.
template <class T>
void warn_reserved(SyntaxBuilder& b, std::string_view name, T value, std::string_view spec_ref = {}) {
  b.diag(Severity::Warning, kReservedCode,
         fmt::format("{}: unknown or reserved value {}", name, static_cast<int64_t>(value)), spec_ref, true);
}

/// rbsp_trailing_bits() (§7.3.2.11) recorded as a struct with rbsp_stop_one_bit f(1) and the
/// rbsp_alignment_zero_bit f(1) run, as trace_headers prints them. Mismatches are SYN-GEN-0001 Diagnostics.
inline void read_rbsp_trailing_bits(BitReader& br, SyntaxBuilder& b) {
  auto s = b.scoped("rbsp_trailing_bits", "H.264 7.3.2.11");
  b.read_f("rbsp_stop_one_bit", 1, 1);
  while (!br.byte_aligned()) {
    b.read_f("rbsp_alignment_zero_bit", 1, 0);
  }
}

/// Ceil(Log2(n)) for n >= 1.
constexpr uint32_t ceil_log2(uint32_t n) noexcept {
  uint32_t bits = 0;
  while ((uint64_t{1} << bits) < n) {
    ++bits;
  }
  return bits;
}

}  // namespace ovt::syntax::h264::detail

#endif  // OVT_SRC_SYNTAX_H264_H264_INTERNAL_H
