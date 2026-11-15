// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 access_unit_delimiter_rbsp() (§7.3.2.4), end_of_seq_rbsp() (§7.3.2.5), end_of_stream_rbsp() (§7.3.2.6) and
// filler_data_rbsp() (§7.3.2.7): chapter 7 §7.3.4 rows 19–20 and §7.3.7, WI-1.109. C++ helper header (D22):
// semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H264_AUD_HPP
#define OVT_CXX_SYNTAX_H264_AUD_HPP

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {
class SyntaxNodeBuffer;
}

namespace ovt::syntax::h264 {

/// access_unit_delimiter_rbsp(), row 19 (nal_unit_type 9).
struct AccessUnitDelimiter {
  uint32_t primary_pic_type = 0;
  // derived
  std::string_view primary_pic_type_name;  // Table 7-5 slice_type values, e.g. "I, P, B" (static storage)
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// filler_data_rbsp(), row 20 (nal_unit_type 12).
struct FillerData {
  uint64_t ff_byte_count = 0;  // derived: the number of ff_byte elements
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Table 7-5 name of primary_pic_type 0..7 (the slice_type values that may be present).
[[nodiscard]] std::string_view primary_pic_type_name(uint32_t primary_pic_type) noexcept;

/// Row 19. Node `access_unit_delimiter_rbsp`. The reader stands after the NAL header.
AccessUnitDelimiter parse_access_unit_delimiter(BitReader& br, SyntaxBuilder& b);

/// Row 20: end_of_seq_rbsp() / end_of_stream_rbsp(), nodes `end_of_seq_rbsp` / `end_of_stream_rbsp`. Both RBSPs are
/// empty; bytes after the NAL header are reported (Diagnostic Warning SYN-H264-0003) and recorded as the opaque
/// `trailing_data` node.
BitSpan parse_end_of_seq(BitReader& br, SyntaxBuilder& b);
BitSpan parse_end_of_stream(BitReader& br, SyntaxBuilder& b);

/// Row 20. Node `filler_data_rbsp`: the ff_byte run as one Bytes node `ff_byte` (descriptor `f(8)*<n>`, n derived),
/// then rbsp_trailing_bits().
FillerData parse_filler_data(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_AUD_HPP
