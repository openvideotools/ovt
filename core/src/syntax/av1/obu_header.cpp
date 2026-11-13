// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 obu_header(), the operating point rule, temporal_delimiter_obu(), padding_obu() and trailing_bits()
// (chapter 7 §7.3.6 rows 1, 7, 8; WI-1.116).

#include "ovt/cxx/syntax/av1/obu_header.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecObuHeader = "AV1 5.3.2";
constexpr std::string_view kSpecTemporalDelimiter = "AV1 5.6";
constexpr std::string_view kSpecPadding = "AV1 5.7";
constexpr std::string_view kSpecTrailingBits = "AV1 5.3.4";

}  // namespace

std::string_view obu_type_name(uint8_t obu_type) noexcept {
  switch (static_cast<ObuType>(obu_type)) {
    case ObuType::SequenceHeader:
      return "OBU_SEQUENCE_HEADER";
    case ObuType::TemporalDelimiter:
      return "OBU_TEMPORAL_DELIMITER";
    case ObuType::FrameHeader:
      return "OBU_FRAME_HEADER";
    case ObuType::TileGroup:
      return "OBU_TILE_GROUP";
    case ObuType::Metadata:
      return "OBU_METADATA";
    case ObuType::Frame:
      return "OBU_FRAME";
    case ObuType::RedundantFrameHeader:
      return "OBU_REDUNDANT_FRAME_HEADER";
    case ObuType::TileList:
      return "OBU_TILE_LIST";
    case ObuType::Padding:
      return "OBU_PADDING";
    case ObuType::Reserved0:
    default:
      return "Reserved";
  }
}

ObuHeader parse_obu_header(BitReader& br, SyntaxBuilder& b) {
  ObuHeader h;
  const auto start = br.mark();
  auto s = b.scoped("obu_header", kSpecObuHeader);
  // AV1 §5.3.2 obu_header()
  h.obu_forbidden_bit = b.read_f("obu_forbidden_bit", 1, 0) != 0;
  h.obu_type = static_cast<uint8_t>(b.read_u("obu_type", 4));
  h.type_name = obu_type_name(h.obu_type);
  b.set_enum(h.type_name);
  if (h.type_name == "Reserved") {
    // SPEC-ASSUMPTION(#321): §7.9 has no code for a reserved obu_type; SYN-AV1-0004 is proposed.
    b.diag(Severity::Warning, "SYN-AV1-0004", fmt::format("obu_type: reserved value {}", h.obu_type), {}, true);
  }
  h.obu_extension_flag = b.read_flag("obu_extension_flag");
  h.obu_has_size_field = b.read_flag("obu_has_size_field");
  h.obu_reserved_1bit = b.read_f("obu_reserved_1bit", 1, 0) != 0;
  if (h.obu_extension_flag) {
    // AV1 §5.3.3 obu_extension_header()
    auto e = b.scoped("obu_extension_header", "AV1 5.3.3");
    h.temporal_id = static_cast<uint8_t>(b.read_u("temporal_id", 3));
    h.spatial_id = static_cast<uint8_t>(b.read_u("spatial_id", 2));
    h.extension_header_reserved_3bits = static_cast<uint8_t>(b.read_f("extension_header_reserved_3bits", 3, 0));
  }
  // AV1 §5.3.1 open_bitstream_unit(): obu_size, or the rest of the unit (sz - 1 - obu_extension_flag)
  if (h.obu_has_size_field) {
    h.obu_size = b.read_leb128("obu_size");
  } else {
    h.obu_size = br.bits_left() / 8;
    b.derived("obu_size", h.obu_size);
  }
  h.header_bits = br.bit_pos() - start;
  h.span = br.span_since(start);
  return h;
}

bool obu_in_operating_point(const ObuHeader& h, uint16_t operating_point_idc) noexcept {
  // AV1 §7.18 (drop process): only OBUs with an extension header can be outside a point; sequence headers,
  // temporal delimiters and padding are never dropped. operating_point_idc == 0 means "all layers".
  const auto t = h.type();
  if (!h.obu_extension_flag || operating_point_idc == 0 || t == ObuType::SequenceHeader ||
      t == ObuType::TemporalDelimiter || t == ObuType::Padding) {
    return true;
  }
  const bool in_temporal_layer = ((operating_point_idc >> h.temporal_id) & 1U) != 0;
  const bool in_spatial_layer = ((operating_point_idc >> (h.spatial_id + 8)) & 1U) != 0;
  return in_temporal_layer && in_spatial_layer;
}

bool check_operating_point(SyntaxBuilder& b, const ObuHeader& h, uint16_t operating_point_idc) {
  if (obu_in_operating_point(h, operating_point_idc)) {
    return true;
  }
  b.diag(Severity::Info, "SYN-AV1-0002", "not in selected operating point", "AV1 7.18");
  return false;
}

void parse_trailing_bits(BitReader& br, SyntaxBuilder& b) {
  auto s = b.scoped("trailing_bits", kSpecTrailingBits);
  (void)b.read_f("trailing_one_bit", 1, 1);
  while (br.bits_left() > 0) {
    const auto n = static_cast<unsigned>(std::min<uint64_t>(br.bits_left(), 64));
    (void)b.read_f("trailing_zero_bits", n, 0);
  }
}

void parse_temporal_delimiter(BitReader& br, SyntaxBuilder& b) {
  {
    auto s = b.scoped("temporal_delimiter_obu", kSpecTemporalDelimiter);  // AV1 §5.6: no syntax elements
  }
  if (br.bits_left() > 0) {
    parse_trailing_bits(br, b);  // AV1 §5.3.1: obu_size > 0 would be followed by trailing bits
  }
}

Padding parse_padding(BitReader& br, SyntaxBuilder& b) {
  Padding p;
  const auto start = br.mark();
  auto s = b.scoped("padding_obu", kSpecPadding);
  p.byte_count = br.bits_left() / 8;
  b.opaque_rest("obu_padding_byte");
  b.derived("padding_bytes", p.byte_count);
  p.span = br.span_since(start);
  return p;
}

}  // namespace ovt::syntax::av1
