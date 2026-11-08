// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the AV1 OBU header, temporal delimiter, metadata, padding and tile list parsers (chapter 7
// §7.3.6 rows 1, 7, 8, 9 and 30, WI-1.116a; the sequence header, rows 2–6, is WI-1.116b).
//
// Expected values come from a recorded oracle run, cited at each fixture: FFmpeg 6.1.1 `-bsf:v trace_headers`
// on the libaom test vectors (bit offsets are those the trace prints, counted from the first OBU header bit),
// and the hand-computed unit conversions of the chapter 14 §14.3.3 recipe 15a MDCV/CLL values. Parity over
// the whole `aom` set and the Argon header subset is WI-1.572.
//
// SPEC-ASSUMPTION(#322): acceptance test 3 says "operating point 0"; operating_point_idc[0] of
// av1-1-b8-22-svc-L2T2 is 0x303 (every layer), so the drop rule is exercised with operating point 3 (0x101)
// and operating point 0 is checked to flag nothing.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/metadata.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/tile_list.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using namespace ovt::syntax::av1;  // NOLINT(google-build-using-namespace)
using ovt::BitReader;
using ovt::ParseError;
using ovt::syntax::DiagnosticData;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
using ovt::syntax::ValueKind;
using Bytes = std::vector<uint8_t>;

// ---- fixtures --------------------------------------------------------------------------------------------

// av1-1-b8-01-size-16x16.ivf, first temporal unit, second OBU (OBU_SEQUENCE_HEADER, obu_size 10). trace_headers:
// 16 seq_profile 000 = 0 … 53 max_frame_width_minus_1 1111 = 15, 57 max_frame_height_minus_1 1111 = 15,
// 74 order_hint_bits_minus_1 110 = 6, 80 high_bitdepth 0, 88 trailing_one_bit 1, 89..95 trailing_zero_bit 0.
const Bytes kSeq16x16{0x0a, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x9f, 0xfb, 0xff, 0xf3, 0x00, 0x80};

class BitWriter {
 public:
  BitWriter& put(uint64_t value, unsigned bits) {
    for (unsigned i = bits; i-- > 0;) {
      if (nbits_ % 8 == 0) {
        bytes_.push_back(0);
      }
      if (((value >> i) & 1U) != 0) {
        bytes_.back() = static_cast<uint8_t>(bytes_.back() | (0x80U >> (nbits_ % 8)));
      }
      ++nbits_;
    }
    return *this;
  }
  // AV1 trailing_bits(): a one bit, then zeros to the byte boundary.
  BitWriter& trailing() {
    put(1, 1);
    while (nbits_ % 8 != 0) {
      put(0, 1);
    }
    return *this;
  }
  [[nodiscard]] const Bytes& bytes() const { return bytes_; }

 private:
  Bytes bytes_;
  unsigned nbits_ = 0;
};

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

// A whole OBU: header byte (type, has_size), leb128 size, payload.
Bytes obu(uint8_t type, const Bytes& payload) {
  Bytes out{static_cast<uint8_t>((type << 3U) | 0x02U)};
  size_t n = payload.size();
  do {
    auto byte = static_cast<uint8_t>(n & 0x7FU);
    n >>= 7U;
    if (n != 0) {
      byte |= 0x80U;
    }
    out.push_back(byte);
  } while (n != 0);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Bytes be16(uint16_t v) {
  return {static_cast<uint8_t>(v >> 8U), static_cast<uint8_t>(v & 0xFFU)};
}
Bytes be32(uint32_t v) {
  return {static_cast<uint8_t>(v >> 24U), static_cast<uint8_t>(v >> 16U), static_cast<uint8_t>(v >> 8U),
          static_cast<uint8_t>(v & 0xFFU)};
}

// Recipe 15a (chapter 14 §14.3.3) as tools/av1_insert_hdr_obus.py codes it: G(13250,34500) B(7500,3000)
// R(34000,16000) WP(15635,16450) L(10000000,1) → 0.16 chromaticities round(v * 0.00002 * 65536) in R, G, B order,
// luminance_max round(10000000 * 0.0001 * 256) = 256000 (24.8), luminance_min round(1 * 0.0001 * 16384) = 2 (18.14).
Bytes mdcv_obu() {
  return obu(5, cat({{0x02},
                     be16(44564),
                     be16(20972),
                     be16(17367),
                     be16(45220),
                     be16(9830),
                     be16(3932),
                     be16(20493),
                     be16(21561),
                     be32(256000),
                     be32(2),
                     {0x80}}));
}
Bytes cll_obu() {
  return obu(5, cat({{0x01}, be16(1000), be16(400), {0x80}}));
}

// ---- tree helpers --------------------------------------------------------------------------------------------

const SyntaxNodeData* find(const SyntaxNodeData* n, std::string_view name, int array_index = -1) {
  if (n == nullptr) {
    return nullptr;
  }
  if (n->name == name && (array_index < 0 || n->array_index == array_index)) {
    return n;
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    if (const SyntaxNodeData* f = find(c, name, array_index); f != nullptr) {
      return f;
    }
  }
  return nullptr;
}

const SyntaxNodeData& node(const SyntaxNodeBuffer& buf, std::string_view name, int array_index = -1) {
  const SyntaxNodeData* n = find(buf.root(), name, array_index);
  REQUIRE(n != nullptr);
  return *n;
}

// The value node of an array whose element wrapper carries the same name (begin_array_element(name, i)).
const SyntaxNodeData& leaf(const SyntaxNodeBuffer& buf, std::string_view name, int array_index) {
  const SyntaxNodeData& wrapper = node(buf, name, array_index);
  const SyntaxNodeData* n = wrapper.kind == ValueKind::None ? find(wrapper.first_child, name, array_index) : &wrapper;
  REQUIRE(n != nullptr);
  return *n;
}

bool span_is(const ovt::BitSpan& s, uint64_t off, uint32_t len) {
  return s.bit_offset == off && s.bit_length == len;
}

// Every diagnostic in the tree with the given code.
std::vector<const DiagnosticData*> diags(const SyntaxNodeData* n, std::string_view code) {
  std::vector<const DiagnosticData*> out;
  if (n == nullptr) {
    return out;
  }
  for (const DiagnosticData* d = n->first_diag; d != nullptr; d = d->next) {
    if (d->code == code) {
      out.push_back(d);
    }
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    const auto sub = diags(c, code);
    out.insert(out.end(), sub.begin(), sub.end());
  }
  return out;
}

// Parses one whole OBU (header first, then the payload parser of its type) and returns the tree.
template <class Parse>
SyntaxNodeBuffer parse_obu(const Bytes& bytes, Parse&& parse, bool recording = true) {
  BitReader br(bytes);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  const ObuHeader h = parse_obu_header(br, b);
  parse(br, b, h);
  SyntaxNodeBuffer buf = b.finish();
  buf.set_root_name("open_bitstream_unit");
  return buf;
}

Metadata parse_meta(const Bytes& bytes, SyntaxNodeBuffer* tree = nullptr) {
  Metadata m;
  SyntaxNodeBuffer buf = parse_obu(bytes, [&](BitReader& br, SyntaxBuilder& b, const ObuHeader& h) {
    REQUIRE(h.type() == ObuType::Metadata);
    m = parse_metadata(br, b);
    REQUIRE(br.bits_left() == 0);
  });
  if (tree != nullptr) {
    *tree = std::move(buf);
  }
  return m;
}

}  // namespace

// ---- obu_header (row 1) -----------------------------------------------------------------------------------------

TEST_CASE("av1_obu_header_fields_and_spans", "[syntax][av1][fast]") {
  BitReader br(kSeq16x16);
  SyntaxBuilder b(br);
  const ObuHeader h = parse_obu_header(br, b);
  CHECK(h.type() == ObuType::SequenceHeader);
  CHECK(h.type_name == "OBU_SEQUENCE_HEADER");
  CHECK_FALSE(h.obu_extension_flag);
  CHECK(h.obu_has_size_field);
  CHECK(h.obu_size == 10);
  CHECK(h.header_bits == 16);
  CHECK(span_is(h.span, 0, 16));
  const SyntaxNodeBuffer buf = b.finish();
  // trace_headers: 0 obu_forbidden_bit, 1 obu_type 0001, 5 obu_extension_flag, 6 obu_has_size_field,
  // 7 obu_reserved_1bit, 8 obu_size.
  CHECK(span_is(node(buf, "obu_forbidden_bit").span, 0, 1));
  const SyntaxNodeData& type = node(buf, "obu_type");
  CHECK(span_is(type.span, 1, 4));
  CHECK(type.uvalue == 1);
  CHECK(type.enum_name == "OBU_SEQUENCE_HEADER");
  CHECK(type.descriptor == "u(4)");
  CHECK(span_is(node(buf, "obu_has_size_field").span, 6, 1));
  const SyntaxNodeData& size = node(buf, "obu_size");
  CHECK(span_is(size.span, 8, 8));
  CHECK(size.descriptor == "leb128()");
  CHECK(node(buf, "obu_header").spec_ref == "AV1 5.3.2");
  CHECK(diags(buf.root(), "SYN-GEN-0001").empty());
}

TEST_CASE("av1_obu_header_extension_and_multibyte_size", "[syntax][av1][fast]") {
  // type 3, extension, has_size; temporal_id 1, spatial_id 1; obu_size 128 as two leb128 bytes.
  const Bytes bytes{0x1e, 0x28, 0x80, 0x01};
  BitReader br(bytes);
  SyntaxBuilder b(br);
  const ObuHeader h = parse_obu_header(br, b);
  CHECK(h.type() == ObuType::FrameHeader);
  CHECK(h.obu_extension_flag);
  CHECK(h.temporal_id == 1);
  CHECK(h.spatial_id == 1);
  CHECK(h.obu_size == 128);
  CHECK(h.header_bits == 32);
  const SyntaxNodeBuffer buf = b.finish();
  CHECK(span_is(node(buf, "temporal_id").span, 8, 3));
  CHECK(span_is(node(buf, "spatial_id").span, 11, 2));
  CHECK(node(buf, "obu_extension_header").spec_ref == "AV1 5.3.3");
  CHECK(span_is(node(buf, "obu_size").span, 16, 16));

  SECTION("operating point rule") {
    CHECK(obu_in_operating_point(h, 0));             // 0 selects every layer
    CHECK(obu_in_operating_point(h, 0x0303));        // L2T2 point 0
    CHECK_FALSE(obu_in_operating_point(h, 0x0301));  // spatial layer 1 excluded
    CHECK_FALSE(obu_in_operating_point(h, 0x0103));  // temporal layer 1 excluded
    ObuHeader seq_hdr = h;
    seq_hdr.obu_type = 1;
    CHECK(obu_in_operating_point(seq_hdr, 0x0101));  // sequence headers are never dropped
    ObuHeader no_ext = h;
    no_ext.obu_extension_flag = false;
    CHECK(obu_in_operating_point(no_ext, 0x0101));
  }
}

TEST_CASE("av1_obu_header_without_size_field_and_reserved_type", "[syntax][av1][fast]") {
  // type 9 (reserved), no extension, no size field, three payload bytes follow.
  const Bytes bytes{0x48, 0xaa, 0xbb, 0xcc};
  BitReader br(bytes);
  SyntaxBuilder b(br);
  const ObuHeader h = parse_obu_header(br, b);
  CHECK(h.type_name == "Reserved");
  CHECK_FALSE(h.obu_has_size_field);
  CHECK(h.obu_size == 3);
  const SyntaxNodeBuffer buf = b.finish();
  CHECK(diags(buf.root(), "SYN-AV1-0004").size() == 1);
  CHECK(node(buf, "obu_size").kind == ValueKind::None);  // derived, not read
}

TEST_CASE("av1_obu_header_forbidden_bit_is_syn_gen_0001", "[syntax][av1][fast]") {
  const Bytes bytes{0x8a, 0x00};
  BitReader br(bytes);
  SyntaxBuilder b(br);
  const ObuHeader h = parse_obu_header(br, b);
  CHECK(h.obu_forbidden_bit);
  const SyntaxNodeBuffer buf = b.finish();
  CHECK(diags(buf.root(), "SYN-GEN-0001").size() == 1);
}

TEST_CASE("av1_obu_type_names", "[syntax][av1][fast]") {
  CHECK(obu_type_name(1) == "OBU_SEQUENCE_HEADER");
  CHECK(obu_type_name(15) == "OBU_PADDING");
  CHECK(obu_type_name(0) == "Reserved");
  CHECK(obu_type_name(9) == "Reserved");
}

// ---- temporal delimiter and padding (rows 7, 8) -----------------------------------------------------------------

TEST_CASE("av1_temporal_delimiter_and_padding", "[syntax][av1][fast]") {
  SECTION("temporal delimiter") {
    const Bytes bytes{0x12, 0x00};
    const SyntaxNodeBuffer buf = parse_obu(bytes, [](BitReader& br, SyntaxBuilder& b, const ObuHeader& h) {
      REQUIRE(h.type() == ObuType::TemporalDelimiter);
      parse_temporal_delimiter(br, b);
      CHECK(br.bits_left() == 0);
    });
    const SyntaxNodeData& td = node(buf, "temporal_delimiter_obu");
    CHECK(td.child_count() == 0);
    CHECK(span_is(td.span, 16, 0));
    CHECK(td.spec_ref == "AV1 5.6");
  }
  SECTION("padding") {
    const Bytes bytes = obu(15, Bytes{0x00, 0x00, 0x00, 0x00, 0x00});
    Padding p;
    const SyntaxNodeBuffer buf = parse_obu(bytes, [&](BitReader& br, SyntaxBuilder& b, const ObuHeader& h) {
      REQUIRE(h.type() == ObuType::Padding);
      p = parse_padding(br, b);
      CHECK(br.bits_left() == 0);
    });
    CHECK(p.byte_count == 5);
    CHECK(span_is(p.span, 16, 40));
    const SyntaxNodeData& bytes_node = node(buf, "obu_padding_byte");
    CHECK(bytes_node.kind == ValueKind::Bytes);
    CHECK(bytes_node.bytes.empty());  // opaque: span only
    CHECK(span_is(bytes_node.span, 16, 40));
  }
}

// ---- metadata_obu (row 9) ---------------------------------------------------------------------------------------

TEST_CASE("av1_metadata_hdr_mdcv_and_cll_units", "[syntax][av1][fast]") {
  // Acceptance test 2 on the OBUs recipe 15a inserts; hand-computed expected values (see mdcv_obu()).
  SyntaxNodeBuffer tree;
  const Metadata mdcv = parse_meta(mdcv_obu(), &tree);
  CHECK(mdcv.metadata_type == 2);
  REQUIRE(mdcv.hdr_mdcv.has_value());
  const MetadataHdrMdcv& m = *mdcv.hdr_mdcv;
  CHECK(m.primary_chromaticity_x[0] == 44564);
  CHECK(m.luminance_max == 256000);
  CHECK(m.luminance_min == 2);
  // ovt_ColorDescription units: R, G, B in 0.00002 (×50000/65536, rounded), luminance in 0.0001 cd/m².
  CHECK(m.mdcv_primaries[0][0] == 34000);
  CHECK(m.mdcv_primaries[0][1] == 16000);
  CHECK(m.mdcv_primaries[1][0] == 13250);
  CHECK(m.mdcv_primaries[1][1] == 34500);
  CHECK(m.mdcv_primaries[2][0] == 7500);
  CHECK(m.mdcv_primaries[2][1] == 3000);
  CHECK(m.mdcv_white_point[0] == 15635);
  CHECK(m.mdcv_white_point[1] == 16450);
  CHECK(m.mdcv_max_luminance == 10000000);  // 256000 * 10000 / 256
  CHECK(m.mdcv_min_luminance == 1);         // 2 * 10000 / 16384 = 1.22 → 1
  CHECK(node(tree, "metadata_type").enum_name == "METADATA_TYPE_HDR_MDCV");
  CHECK(span_is(node(tree, "primary_chromaticity_x", 0).span, 24, 16));
  CHECK(span_is(node(tree, "primary_chromaticity_y", 2).span, 24 + 5 * 16, 16));
  CHECK(span_is(node(tree, "luminance_min").span, 24 + 8 * 16 + 32, 32));
  CHECK(node(tree, "metadata_hdr_mdcv").spec_ref == "AV1 5.8.4");
  CHECK(span_is(node(tree, "trailing_one_bit").span, 24 + 8 * 16 + 64, 1));

  const Metadata cll = parse_meta(cll_obu());
  REQUIRE(cll.hdr_cll.has_value());
  CHECK(cll.hdr_cll->max_cll == 1000);
  CHECK(cll.hdr_cll->max_fall == 400);

  ovt_ColorDescription cd{};
  apply_metadata(mdcv, cd);
  apply_metadata(cll, cd);
  CHECK(cd.mdcv_primaries[0][0] == 34000);
  CHECK(cd.mdcv_primaries[1][1] == 34500);
  CHECK(cd.mdcv_primaries[2][0] == 7500);
  CHECK(cd.mdcv_white_point[1] == 16450);
  CHECK(cd.mdcv_max_luminance == 10000000);
  CHECK(cd.mdcv_min_luminance == 1);
  CHECK(cd.max_cll == 1000);
  CHECK(cd.max_fall == 400);
  CHECK(cd.has_hdr10plus == 0);
}

TEST_CASE("av1_metadata_itut_t35_hdr10plus_and_dolby", "[syntax][av1][fast]") {
  SECTION("HDR10+ (registry row 0x003C / 0x0001)") {
    // ST 2094-40 body with one window (the WI-1.120 fixture): application_identifier 4, version 1, maxscl
    // 1000/2000/3000, tone mapping and colour saturation present.
    BitWriter body;
    body.put(4, 8).put(1, 8).put(1, 2).put(4000, 27).put(0, 1);
    body.put(1000, 17).put(2000, 17).put(3000, 17).put(1500, 17).put(3, 4);
    body.put(1, 7).put(100, 17).put(50, 7).put(900, 17).put(99, 7).put(2800, 17);
    body.put(25, 10).put(0, 1);
    body.put(1, 1).put(0x123, 12).put(0x456, 12).put(2, 4).put(0x155, 10).put(0x2AA, 10);
    body.put(1, 1).put(33, 6);
    const Bytes bytes = obu(5, cat({{0x04, 0xB5, 0x00, 0x3C, 0x00, 0x01}, body.bytes(), {0x80}}));
    SyntaxNodeBuffer tree;
    const Metadata m = parse_meta(bytes, &tree);
    CHECK(m.metadata_type == 4);
    REQUIRE(m.itut_t35.has_value());
    const MetadataItutT35& t = *m.itut_t35;
    CHECK(t.itu_t_t35_country_code == 0xB5);
    CHECK_FALSE(t.itu_t_t35_country_code_extension_byte.has_value());
    REQUIRE(t.itu_t_t35_terminal_provider_code.has_value());
    CHECK(*t.itu_t_t35_terminal_provider_code == 0x003C);
    REQUIRE(t.itu_t_t35_terminal_provider_oriented_code.has_value());
    CHECK(*t.itu_t_t35_terminal_provider_oriented_code == 0x0001);
    CHECK(t.enum_name == "HDR10+ (SMPTE ST 2094-40)");
    CHECK(t.has_hdr10plus);
    CHECK_FALSE(t.has_dolby_vision);
    CHECK(t.t35_message.size() == 5 + body.bytes().size());  // trailing 0x80 stripped
    CHECK(node(tree, "itu_t_t35_terminal_provider_code").enum_name == "HDR10+ (SMPTE ST 2094-40)");
    CHECK(node(tree, "itu_t_t35_terminal_provider_oriented_code").descriptor == "u(16)");
    const SyntaxNodeData& pb = node(tree, "itu_t_t35_payload_bytes");
    CHECK(pb.kind == ValueKind::Bytes);
    CHECK(pb.bytes.size() == body.bytes().size());
    CHECK(span_is(node(tree, "trailing_one_bit").span, 24 + 40 + body.bytes().size() * 8, 1));
    ovt_ColorDescription cd{};
    apply_metadata(m, cd);
    CHECK(cd.has_hdr10plus == 1);
  }
  SECTION("Dolby Vision RPU (32-bit provider-oriented code)") {
    const Bytes payload{0x04, 0xB5, 0x00, 0x3B, 0x00, 0x00, 0x08, 0x00, 0x19, 0x01, 0x80};
    SyntaxNodeBuffer tree;
    const Metadata m = parse_meta(obu(5, payload), &tree);
    REQUIRE(m.itut_t35.has_value());
    CHECK(m.itut_t35->has_dolby_vision);
    CHECK(*m.itut_t35->itu_t_t35_terminal_provider_oriented_code == 0x00000800);
    CHECK(node(tree, "itu_t_t35_terminal_provider_oriented_code").descriptor == "u(32)");
    CHECK(node(tree, "itu_t_t35_payload_bytes").bytes.size() == 2);
  }
  SECTION("unknown country has no provider field") {
    const Bytes payload{0x04, 0xFF, 0x12, 0xAA, 0xBB, 0x80};
    SyntaxNodeBuffer tree;
    const Metadata m = parse_meta(obu(5, payload), &tree);
    REQUIRE(m.itut_t35.has_value());
    CHECK(m.itut_t35->itu_t_t35_country_code == 0xFF);
    CHECK(m.itut_t35->itu_t_t35_country_code_extension_byte == 0x12);
    CHECK_FALSE(m.itut_t35->itu_t_t35_terminal_provider_code.has_value());
    CHECK(node(tree, "itu_t_t35_payload_bytes").bytes.size() == 2);
  }
}

TEST_CASE("av1_metadata_scalability_and_timecode", "[syntax][av1][fast]") {
  SECTION("scalability_structure") {
    BitWriter w;
    w.put(14, 8);                                            // SCALABILITY_SS
    w.put(1, 2).put(1, 1).put(1, 1).put(1, 1).put(0, 3);     // 2 spatial layers, all three tables, reserved 0
    w.put(640, 16).put(360, 16).put(1280, 16).put(720, 16);  // dimensions
    w.put(0, 8).put(1, 8);                                   // ref ids
    w.put(2, 8);                                             // temporal_group_size
    w.put(0, 3).put(1, 1).put(0, 1).put(1, 3).put(2, 8);     // group 0: tid 0, ref_cnt 1, diff 2
    w.put(1, 3).put(0, 1).put(1, 1).put(0, 3);               // group 1: tid 1, no refs
    w.trailing();
    SyntaxNodeBuffer tree;
    const Metadata m = parse_meta(obu(5, cat({{0x03}, w.bytes()})), &tree);
    REQUIRE(m.scalability.has_value());
    CHECK(m.scalability->scalability_mode_idc == 14);
    REQUIRE(m.scalability->scalability_structure.has_value());
    const ScalabilityStructure& ss = *m.scalability->scalability_structure;
    CHECK(ss.spatial_layers_cnt_minus_1 == 1);
    REQUIRE(ss.spatial_layer_max_width.size() == 2);
    CHECK(ss.spatial_layer_max_width[1] == 1280);
    CHECK(ss.spatial_layer_max_height[0] == 360);
    REQUIRE(ss.spatial_layer_ref_id.size() == 2);
    CHECK(ss.spatial_layer_ref_id[1] == 1);
    CHECK(ss.temporal_group_size == 2);
    REQUIRE(ss.temporal_groups.size() == 2);
    CHECK(ss.temporal_groups[0].temporal_group_temporal_switching_up_point_flag);
    REQUIRE(ss.temporal_groups[0].temporal_group_ref_pic_diff.size() == 1);
    CHECK(ss.temporal_groups[0].temporal_group_ref_pic_diff[0] == 2);
    CHECK(ss.temporal_groups[1].temporal_group_temporal_id == 1);
    CHECK(node(tree, "scalability_mode_idc").enum_name == "SCALABILITY_SS");
    CHECK(node(tree, "spatial_layer_max_width", 1).uvalue == 1280);
    CHECK(leaf(tree, "temporal_group_ref_pic_diff", 0).uvalue == 2);
    CHECK(diags(tree.root(), "SYN-AV1-0004").empty());
  }
  SECTION("reserved scalability_mode_idc") {
    BitWriter w;
    w.put(200, 8).trailing();
    SyntaxNodeBuffer tree;
    const Metadata m = parse_meta(obu(5, cat({{0x03}, w.bytes()})), &tree);
    REQUIRE(m.scalability.has_value());
    CHECK_FALSE(m.scalability->scalability_structure.has_value());
    CHECK(diags(tree.root(), "SYN-AV1-0004").size() == 1);
  }
  SECTION("timecode, full timestamp") {
    BitWriter w;
    w.put(1, 5).put(1, 1).put(0, 1).put(0, 1).put(12, 9);  // counting_type 1, full, n_frames 12
    w.put(34, 6).put(56, 6).put(7, 5).put(4, 5).put(9, 4).trailing();
    SyntaxNodeBuffer tree;
    const Metadata m = parse_meta(obu(5, cat({{0x05}, w.bytes()})), &tree);
    REQUIRE(m.timecode.has_value());
    CHECK(m.timecode->n_frames == 12);
    CHECK(m.timecode->hours_value == 7);
    CHECK(m.timecode->time_offset_length == 4);
    CHECK(m.timecode->time_offset_value == 9);
    CHECK(m.timecode->timecode_text == "07:56:34:12");
    CHECK(node(tree, "time_offset_value").descriptor == "f(n)");
  }
  SECTION("timecode, seconds only") {
    BitWriter w;
    w.put(0, 5).put(0, 1).put(0, 1).put(0, 1).put(3, 9);
    w.put(1, 1).put(45, 6).put(0, 1).put(0, 5).trailing();  // seconds_flag, 45, minutes_flag 0, offset len 0
    const Metadata m = parse_meta(obu(5, cat({{0x05}, w.bytes()})));
    REQUIRE(m.timecode.has_value());
    CHECK(m.timecode->seconds_flag);
    CHECK_FALSE(m.timecode->minutes_flag);
    CHECK(m.timecode->timecode_text == "--:--:45:03");
  }
}

TEST_CASE("av1_metadata_unknown_type_is_info_not_error", "[syntax][av1][fast]") {
  const Bytes bytes = obu(5, Bytes{0x07, 0xDE, 0xAD, 0xBE, 0xEF, 0x80});  // user private type 7
  SyntaxNodeBuffer tree;
  const Metadata m = parse_meta(bytes, &tree);  // `bytes` outlives m: unknown_payload views it
  CHECK(m.metadata_type == 7);
  CHECK_FALSE(m.known_type());
  REQUIRE(m.unknown_payload.size() == 4);
  CHECK(m.unknown_payload[0] == 0xDE);
  const SyntaxNodeData& u = node(tree, "metadata_unknown");
  CHECK(find(&u, "payload_type") != nullptr);
  CHECK(find(&u, "payload_size") != nullptr);
  CHECK(node(tree, "bytes").bytes.size() == 4);
  CHECK(diags(tree.root(), "SYN-AV1-0005").size() == 1);
  CHECK(diags(tree.root(), "SYN-GEN-0001").empty());
  CHECK(span_is(node(tree, "trailing_one_bit").span, 24 + 32, 1));
}

TEST_CASE("av1_metadata_truncated_is_parse_error_with_partial_tree", "[syntax][av1][fast]") {
  const Bytes bytes = obu(5, Bytes{0x01, 0x03});  // HDR CLL cut inside max_cll
  BitReader br(bytes);
  SyntaxBuilder b(br);
  (void)parse_obu_header(br, b);
  CHECK_THROWS_AS(parse_metadata(br, b), ParseError);
  const SyntaxNodeBuffer buf = b.finish();
  CHECK(node(buf, "metadata_type").uvalue == 1);
  CHECK(find(buf.root(), "max_cll") == nullptr);
}

// ---- tile_list_obu (row 30) -----------------------------------------------------------------------------------

TEST_CASE("av1_tile_list_entries", "[syntax][av1][fast]") {
  // 2x1 output, two tiles: (anchor 0, row 1, col 2, 3 bytes) and (anchor 5, row 0, col 0, 1 byte).
  const Bytes payload{0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x02, 0x02, 0x00,
                      0xaa, 0xbb, 0xcc, 0x05, 0x00, 0x00, 0x00, 0x00, 0xdd};
  TileList t;
  const SyntaxNodeBuffer buf = parse_obu(obu(8, payload), [&](BitReader& br, SyntaxBuilder& b, const ObuHeader& h) {
    REQUIRE(h.type() == ObuType::TileList);
    t = parse_tile_list(br, b);
    CHECK(br.bits_left() == 0);
  });
  CHECK(t.output_frame_width_in_tiles_minus_1 == 1);
  CHECK(t.output_frame_height_in_tiles_minus_1 == 0);
  CHECK(t.tile_count_minus_1 == 1);
  REQUIRE(t.entries.size() == 2);
  CHECK(t.entries[0].anchor_tile_row == 1);
  CHECK(t.entries[0].anchor_tile_col == 2);
  CHECK(t.entries[0].tile_data_size_minus_1 == 2);
  CHECK(span_is(t.entries[0].coded_tile_data, 16 + 32 + 40, 24));
  CHECK(t.entries[1].anchor_frame_idx == 5);
  CHECK(span_is(t.entries[1].coded_tile_data, 16 + 32 + 40 + 24 + 40, 8));
  CHECK(node(buf, "tile_data_size_minus_1", 0).descriptor == "le(2)");
  CHECK(node(buf, "coded_tile_data", 1).bytes.size() == 1);
  CHECK(node(buf, "tile_list_entry", 1).spec_ref == "AV1 5.12.2");
  CHECK(diags(buf.root(), "SYN-AV1-0006").size() == 1);

  // A tile_data_size that overruns the OBU is a BitReader error.
  const Bytes bad = obu(8, Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xaa});
  BitReader br(bad);
  SyntaxBuilder b(br);
  (void)parse_obu_header(br, b);
  try {
    (void)parse_tile_list(br, b);
    FAIL("expected ParseError");
  } catch (const ParseError& e) {
    CHECK(e.code == "BR-0001");
  }
}
