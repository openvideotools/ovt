// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 access unit delimiter, end of sequence, end of bitstream and filler data RBSPs (chapter 7 §7.3.5 row 15,
// WI-1.112 follow-up). Element order follows H.265 (V10, 09/2023) §7.3.2.5–7.3.2.8.

#include "ovt/cxx/syntax/h265/aud.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

constexpr std::string_view kTrailingBytesCode = "SYN-H265-0033";

BitSpan parse_empty_rbsp(BitReader& br, SyntaxBuilder& b, std::string_view name, std::string_view spec_ref) {
  const auto start = br.mark();
  auto scope = b.scoped(name, spec_ref);
  if (br.bits_left() > 0) {
    // SPEC-ASSUMPTION(#662): Info SYN-H265-0033 "%s has %d trailing bytes after the NAL unit header", the H.265
    // counterpart of SYN-H264-0033 (§7.3.4 row 20, #551); the bytes are kept as the opaque node `trailing_data`.
    b.opaque_rest("trailing_data");
    b.diag(Severity::Info, kTrailingBytesCode,
           fmt::format("{} has {} trailing bytes after the NAL unit header", name, (br.bits_left() + 7) / 8), spec_ref,
           true);
  }
  return br.span_since(start);
}

}  // namespace

std::string_view pic_type_name(uint32_t pic_type) noexcept {
  switch (pic_type) {  // Table 7-2
    case 0:
      return "I";
    case 1:
      return "P, I";
    case 2:
      return "B, P, I";
    default:
      return {};
  }
}

AccessUnitDelimiter parse_access_unit_delimiter(BitReader& br, SyntaxBuilder& b) {
  AccessUnitDelimiter aud;
  const auto start = br.mark();
  auto scope = b.scoped("access_unit_delimiter_rbsp", "H.265 7.3.2.5");
  aud.pic_type = static_cast<uint32_t>(b.read_u("pic_type", 3));
  aud.pic_type_name = pic_type_name(aud.pic_type);
  if (detail::check_range(b, "pic_type", aud.pic_type, 0, 2, "H.265 7.4.3.5")) {  // 3..7 are reserved
    b.set_enum(aud.pic_type_name);
  }
  detail::read_rbsp_trailing_bits(br, b);
  aud.span = br.span_since(start);
  return aud;
}

BitSpan parse_end_of_seq(BitReader& br, SyntaxBuilder& b) {
  return parse_empty_rbsp(br, b, "end_of_seq_rbsp", "H.265 7.3.2.6");
}

BitSpan parse_end_of_bitstream(BitReader& br, SyntaxBuilder& b) {
  return parse_empty_rbsp(br, b, "end_of_bitstream_rbsp", "H.265 7.3.2.7");
}

FillerData parse_filler_data(BitReader& br, SyntaxBuilder& b) {
  FillerData f;
  const auto start = br.mark();
  auto scope = b.scoped("filler_data_rbsp", "H.265 7.3.2.8");
  // while (next_bits(8) == 0xFF) ff_byte f(8): the run is counted first and recorded as one node, so a large
  // filler unit costs one node, not one per byte. The reader is byte aligned after the 16-bit NAL unit header.
  if (br.byte_aligned()) {
    const auto rest = br.remaining_bytes();
    uint64_t n = 0;
    while (n < rest.size() && rest[n] == 0xFF) {
      ++n;
    }
    f.ff_byte_count = n;
    if (n > 0) {
      (void)b.read_bytes("ff_byte", static_cast<size_t>(n));
      b.set_descriptor_text(fmt::format("f(8)*{}", n));
    }
  }
  b.derived("ff_byte_count", f.ff_byte_count);
  detail::read_rbsp_trailing_bits(br, b);
  f.span = br.span_since(start);
  return f;
}

}  // namespace ovt::syntax::h265
