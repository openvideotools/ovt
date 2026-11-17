// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 access_unit_delimiter_rbsp() (§7.3.2.5), end_of_seq_rbsp() (§7.3.2.6), end_of_bitstream_rbsp() (§7.3.2.7)
// and filler_data_rbsp() (§7.3.2.8): chapter 7 §7.3.5 row 15 and §7.3.7, WI-1.112 (follow-up; FR-SYN-005). C++
// helper header (D22): semi-public, no ABI promise. Mirrors the H.264 header ovt/cxx/syntax/h264/aud.hpp.

#ifndef OVT_CXX_SYNTAX_H265_AUD_HPP
#define OVT_CXX_SYNTAX_H265_AUD_HPP

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {
class SyntaxNodeBuffer;
}

namespace ovt::syntax::h265 {

/// access_unit_delimiter_rbsp(), row 15 (nal_unit_type 35, AUD_NUT).
struct AccessUnitDelimiter {
  uint32_t pic_type = 0;
  // derived
  std::string_view pic_type_name;  // Table 7-2 slice_type values, e.g. "B, P, I"; empty for the reserved 3..7
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// filler_data_rbsp(), row 15 (nal_unit_type 38, FD_NUT).
struct FillerData {
  uint64_t ff_byte_count = 0;  // derived: the number of ff_byte elements
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Table 7-2 name of pic_type 0..2 (the slice_type values that may be present); empty for the reserved 3..7.
[[nodiscard]] std::string_view pic_type_name(uint32_t pic_type) noexcept;

/// Row 15. Node `access_unit_delimiter_rbsp`. The reader stands after the NAL unit header. A reserved pic_type
/// (3..7) is Diagnostic Error SYN-H265-0007; parsing continues.
AccessUnitDelimiter parse_access_unit_delimiter(BitReader& br, SyntaxBuilder& b);

/// Row 15: end_of_seq_rbsp() / end_of_bitstream_rbsp(), nodes `end_of_seq_rbsp` / `end_of_bitstream_rbsp`. Both
/// RBSPs are empty; bytes after the NAL unit header are reported (Info SYN-H265-0033, #662) and recorded as the
/// opaque `trailing_data` node.
BitSpan parse_end_of_seq(BitReader& br, SyntaxBuilder& b);
BitSpan parse_end_of_bitstream(BitReader& br, SyntaxBuilder& b);

/// Row 15. Node `filler_data_rbsp`: the ff_byte run as one Bytes node `ff_byte` (descriptor `f(8)*<n>`, n derived),
/// then rbsp_trailing_bits().
FillerData parse_filler_data(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_AUD_HPP
