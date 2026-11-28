// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 access unit delimiter, end of sequence, end of stream and filler data RBSPs (chapter 7 §7.3.4 rows 19–20,
// WI-1.109). Element order follows H.264 (08/2021) §7.3.2.4–7.3.2.7.

#include "ovt/cxx/syntax/h264/aud.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

constexpr std::string_view kTrailingBytesCode = "SYN-H264-0033";

BitSpan parse_empty_rbsp(BitReader& br, SyntaxBuilder& b, std::string_view name, std::string_view spec_ref) {
  const auto start = br.mark();
  auto scope = b.scoped(name, spec_ref);
  if (br.bits_left() > 0) {
    // SPEC-ASSUMPTION(#551): Info SYN-H264-0033 "%s has %d trailing bytes after the NAL unit header" (accepted,
    // §7.9 row lands in rc34).
    // Count before opaque_rest consumes the bytes, or the message always says 0.
    const auto trailing_bytes = (br.bits_left() + 7) / 8;
    b.opaque_rest("trailing_data");
    b.diag(Severity::Info, kTrailingBytesCode,
           fmt::format("{} has {} trailing bytes after the NAL unit header", name, trailing_bytes), spec_ref, true);
  }
  return br.span_since(start);
}

}  // namespace

std::string_view primary_pic_type_name(uint32_t primary_pic_type) noexcept {
  switch (primary_pic_type) {  // Table 7-5
    case 0:
      return "I";
    case 1:
      return "I, P";
    case 2:
      return "I, P, B";
    case 3:
      return "SI";
    case 4:
      return "SI, SP";
    case 5:
      return "I, SI";
    case 6:
      return "I, SI, P, SP";
    case 7:
      return "I, SI, P, SP, B";
    default:
      return {};
  }
}

AccessUnitDelimiter parse_access_unit_delimiter(BitReader& br, SyntaxBuilder& b) {
  AccessUnitDelimiter aud;
  const auto start = br.mark();
  auto scope = b.scoped("access_unit_delimiter_rbsp", "H.264 7.3.2.4");
  aud.primary_pic_type = static_cast<uint32_t>(b.read_u("primary_pic_type", 3));
  aud.primary_pic_type_name = primary_pic_type_name(aud.primary_pic_type);
  b.set_enum(aud.primary_pic_type_name);
  detail::read_rbsp_trailing_bits(br, b);
  aud.span = br.span_since(start);
  return aud;
}

BitSpan parse_end_of_seq(BitReader& br, SyntaxBuilder& b) {
  return parse_empty_rbsp(br, b, "end_of_seq_rbsp", "H.264 7.3.2.5");
}

BitSpan parse_end_of_stream(BitReader& br, SyntaxBuilder& b) {
  return parse_empty_rbsp(br, b, "end_of_stream_rbsp", "H.264 7.3.2.6");
}

FillerData parse_filler_data(BitReader& br, SyntaxBuilder& b) {
  FillerData f;
  const auto start = br.mark();
  auto scope = b.scoped("filler_data_rbsp", "H.264 7.3.2.7");
  // while (next_bits(8) == 0xFF) ff_byte f(8): the run is counted first and recorded as one node, so a large
  // filler unit costs one node, not one per byte.
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

}  // namespace ovt::syntax::h264
