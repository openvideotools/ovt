// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 obu_header() with obu_extension_header() and obu_size (AV1 §5.3.1–§5.3.3), the operating point drop
// rule (AV1 §7.18), temporal_delimiter_obu() (§5.6), padding_obu() (§5.7) and trailing_bits() (§5.3.4):
// chapter 7 §7.3.6 rows 1, 7 and 8, WI-1.116. C++ helper header (D22): semi-public, no ABI promise, never
// included from a C header, not installed.
//
// Every parser reads through the SyntaxBuilder (§7.3.1): one read_* per syntax element in spec order, the spec
// element name as the node name. The BitReader spans one OBU as the unit scanner hands it out (header,
// obu_size field and payload, chapter 7 §7.3.3), so the end of the reader is the end of the OBU; bit offsets
// count from the first header bit. ParseError propagates (§7.3.1 rule 1).

#ifndef OVT_CXX_SYNTAX_AV1_OBU_HEADER_HPP
#define OVT_CXX_SYNTAX_AV1_OBU_HEADER_HPP

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::av1 {

/// obu_type values (AV1 §6.2.2).
enum class ObuType : uint8_t {
  Reserved0 = 0,
  SequenceHeader = 1,
  TemporalDelimiter = 2,
  FrameHeader = 3,
  TileGroup = 4,
  Metadata = 5,
  Frame = 6,
  RedundantFrameHeader = 7,
  TileList = 8,
  Padding = 15,
};

/// "OBU_SEQUENCE_HEADER" … "OBU_PADDING"; "Reserved" for the other values.
[[nodiscard]] std::string_view obu_type_name(uint8_t obu_type) noexcept;

/// §7.3.6 row 1: obu_header(), obu_extension_header() and obu_size.
struct ObuHeader {
  bool obu_forbidden_bit = false;
  uint8_t obu_type = 0;
  bool obu_extension_flag = false;
  bool obu_has_size_field = false;
  bool obu_reserved_1bit = false;
  // obu_extension_header(), zero when obu_extension_flag == 0 (AV1 §6.2.3: a missing extension means layer 0)
  uint8_t temporal_id = 0;
  uint8_t spatial_id = 0;
  uint8_t extension_header_reserved_3bits = 0;
  // Payload size in bytes: the obu_size field, or the bytes left in the reader when the field is absent.
  uint64_t obu_size = 0;
  // Derived
  std::string_view type_name;  // obu_type_name(obu_type)
  uint64_t header_bits = 0;    // bits consumed by the header, the extension and the size field
  BitSpan span{0, 0};

  [[nodiscard]] ObuType type() const noexcept { return static_cast<ObuType>(obu_type); }
};

/// Parses obu_header(), the extension header and obu_size at the current position (node "obu_header").
/// A set obu_forbidden_bit is Diagnostic Error SYN-GEN-0001 (read_f); a reserved obu_type is a Warning
/// (SPEC-ASSUMPTION(#321): SYN-AV1-0004); the reserved bits are read with read_f and reported through
/// SYN-GEN-0001 when set. Nothing here throws except a truncated OBU (BR-0001).
[[nodiscard]] ObuHeader parse_obu_header(BitReader& br, SyntaxBuilder& b);

/// Operating point drop rule (§7.3.6 row 1, AV1 §7.18): true when the OBU belongs to the operating point
/// whose operating_point_idc is `operating_point_idc`. OBUs without an extension header, sequence headers,
/// temporal delimiters and padding always belong; operating_point_idc == 0 selects every layer.
[[nodiscard]] bool obu_in_operating_point(const ObuHeader& h, uint16_t operating_point_idc) noexcept;

/// obu_in_operating_point(), and when false attaches Diagnostic Info SYN-AV1-0002 "not in selected operating
/// point" to the current struct. The caller then indexes the OBU with FrameInfo.shown = false (§7.4.3).
bool check_operating_point(SyntaxBuilder& b, const ObuHeader& h, uint16_t operating_point_idc);

/// §7.3.6 row 7: temporal_delimiter_obu() is empty (node "temporal_delimiter_obu"); the caller resets
/// SeenFrameHeader (§7.3.9). Reads trailing_bits() when payload bits remain (AV1 §5.3.1).
void parse_temporal_delimiter(BitReader& br, SyntaxBuilder& b);

/// §7.3.6 row 8: padding_obu().
struct Padding {
  uint64_t byte_count = 0;  // derived: obu_padding_byte[] length
  BitSpan span{0, 0};
};
/// The remaining bytes of the OBU become the opaque node "obu_padding_byte" (kind Bytes, no copy).
[[nodiscard]] Padding parse_padding(BitReader& br, SyntaxBuilder& b);

/// trailing_bits() (AV1 §5.3.4) over every bit left in the reader: trailing_one_bit f(1) = 1, then
/// trailing_zero_bits f(n) = 0 in runs of at most 64 bits. A wrong value is Diagnostic Error SYN-GEN-0001
/// (read_f) and parsing continues; an OBU with no bit left raises BR-0001 as the spec's read would.
void parse_trailing_bits(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::av1

#endif  // OVT_CXX_SYNTAX_AV1_OBU_HEADER_HPP
