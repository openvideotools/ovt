// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of FieldBuilder (chapter 08 §8.2.8, WI-1.202) on synthetic payloads.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/bitreader.hpp"

namespace {

using Bytes = std::vector<uint8_t>;
namespace iso = ovt::container::isobmff;
using ovt::container::Severity;
using ovt::fb::SyntaxValueKind;

constexpr unsigned kHeaderBytes = 8;
constexpr uint64_t kHeaderBits = uint64_t{kHeaderBytes} * 8U;

// A box node with a plain 8-byte header at file offset 100.
ovt::fb::BoxNodeT make_node(const char* fourcc = "test") {
  ovt::fb::BoxNodeT n;
  n.fourcc = fourcc;
  n.header_length = kHeaderBytes;
  n.file_span = std::make_unique<ovt::fb::ByteSpan>(100, 100);
  return n;
}

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}

const ovt::fb::FieldT& field(const ovt::fb::BoxNodeT& n, size_t i) {
  REQUIRE(i < n.fields.size());
  return *n.fields[i];
}

}  // namespace

TEST_CASE("FieldBuilder records spans relative to the box start", "[container][fieldbuilder]") {
  const Bytes payload = {0, 0, 0, 1, 0, 0, 0, 2};
  ovt::BitReader br(payload);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br);
  CHECK(fb.u32("a") == 1);
  CHECK(fb.u32("b") == 2);
  REQUIRE(node.fields.size() == 2);
  // {0,32} and {32,32} from the payload start, plus the 8 header bytes.
  CHECK(field(node, 0).span->bit_offset() == kHeaderBits + 0);
  CHECK(field(node, 0).span->bit_length() == 32);
  CHECK(field(node, 1).span->bit_offset() == kHeaderBits + 32);
  CHECK(field(node, 1).span->bit_length() == 32);
  CHECK(fb.bytes_consumed() == 8);
}

TEST_CASE("FieldBuilder integer readers and sub-byte spans", "[container][fieldbuilder]") {
  // 3 bits 0b101, 5 bits 0b00111, then i16 -2, u24, i32or64 narrow, u32or64 wide
  Bytes p = {0b10100111, 0xFF, 0xFE, 0x01, 0x02, 0x03};
  put32(p, 0xFFFFFFFEU);
  for (int i = 0; i < 7; ++i) {
    p.push_back(0);
  }
  p.push_back(9);
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  CHECK(fb.u("hi", 3) == 5);
  CHECK(fb.u("lo", 5, "u(5)") == 7);
  CHECK(fb.i16("neg") == -2);
  CHECK(fb.u24("three") == 0x010203);
  CHECK(fb.i32or64("s32", false) == -2);
  CHECK(fb.u32or64("u64", true) == 9);
  CHECK(field(node, 1).span->bit_offset() == 3);  // crosses no byte boundary, but starts mid-byte
  CHECK(field(node, 2).ivalue == -2);
}

TEST_CASE("FieldBuilder fixed point", "[container][fieldbuilder]") {
  Bytes p;
  put32(p, 0x00010000);             // 1.0
  put32(p, 0x00008000);             // 0.5
  put32(p, 0xFFFF0000);             // -1.0 signed
  p.insert(p.end(), {0x01, 0x00});  // 8.8 1.0
  put32(p, 0x40000000);             // 2.30 1.0
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  CHECK(fb.fixed16_16("one") == 1.0);
  CHECK(fb.fixed16_16("half") == 0.5);
  CHECK(fb.fixed16_16("minus") == -1.0);
  CHECK(fb.fixed8_8("vol") == 1.0);
  CHECK(fb.fixed2_30("w") == 1.0);
  CHECK(field(node, 0).kind == SyntaxValueKind::Fixed);
  CHECK(field(node, 0).ivalue == 0x10000);
  CHECK(field(node, 0).svalue == "1.0");
  CHECK(field(node, 2).svalue == "-1.0");
  CHECK(field(node, 4).svalue == "1.0");
}

TEST_CASE("FieldBuilder fourcc, strings and blobs", "[container][fieldbuilder]") {
  Bytes p = {'i', 's', 'o', 'm', 'h', 'i', 0, 'x', 'a', 'b', 'c', 0, 0, 1, 2, 3, 4, 5};
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  const auto cc = fb.fourcc("major_brand");
  CHECK(std::string(cc.data(), 4) == "isom");
  CHECK(fb.str_nul("name", 64) == "hi");
  CHECK(field(node, 0).kind == SyntaxValueKind::String);
  CHECK(field(node, 0).svalue == "isom");
  CHECK(field(node, 1).span->bit_length() == 24);  // 'h' 'i' NUL
  CHECK(fb.str_fixed("fixed", 6) == "xabc");
  const auto blob = fb.bytes("Data", 3);
  REQUIRE(blob.size() == 3);
  CHECK(blob[2] == 3);
  CHECK(field(node, 3).bytes == Bytes{1, 2, 3});
  CHECK_THROWS_AS(fb.bytes("too_long", 9), ovt::ParseError);
  CHECK(node.fields.size() == 4);
}

TEST_CASE("FieldBuilder bytes keeps a prefix of a huge blob", "[container][fieldbuilder]") {
  const Bytes p((1U << 20U) + 5, 7);
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  CHECK(fb.bytes("big", (1U << 20U) + 1).size() == (1U << 20U) + 1);
  CHECK(field(node, 0).bytes.size() == 4096);
  CHECK(field(node, 0).uvalue == (1U << 20U) + 1);
  CHECK_FALSE(field(node, 0).svalue.empty());
  CHECK(field(node, 0).span->bit_length() == uint64_t{(1U << 20U) + 1} * 8U);
  CHECK(fb.bytes("small", 4).size() == 4);
  CHECK(field(node, 1).bytes.size() == 4);
}

TEST_CASE("FieldBuilder lang_packed", "[container][fieldbuilder]") {
  const Bytes p = {0x15, 0xC7, 0x55, 0xC4, 0x00, 0x00, 0x95, 0xC7};
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  CHECK(fb.lang_packed("language") == "eng");
  CHECK(fb.lang_packed("language") == "und");
  CHECK(fb.lang_packed("language") == "und");
  CHECK(fb.lang_packed("language") == "und");  // pad bit set
}

TEST_CASE("FieldBuilder reserved", "[container][fieldbuilder]") {
  const Bytes p = {0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  fb.reserved(16);  // fine
  CHECK(node.diagnostics.empty());
  fb.reserved(16);  // 0x0005 != 0
  REQUIRE(node.diagnostics.size() == 1);
  CHECK(node.diagnostics[0]->code == "CNT-1007");
  CHECK(node.diagnostics[0]->severity == ovt::fb::Severity::Info);
  CHECK(node.diagnostics[0]->span->bit_offset() == 16);
  fb.reserved(72);  // wider than 64 bits
  CHECK(node.diagnostics.size() == 1);
  CHECK(field(node, 0).name == "reserved");
}

TEST_CASE("FieldBuilder datetime1904", "[container][fieldbuilder]") {
  Bytes p;
  put32(p, 0);
  put32(p, 2082844800U);  // 1970-01-01
  put32(p, 3034627200U);  // 2000-02-29 (leap day)
  for (int i = 0; i < 8; ++i) {
    p.push_back(0xFF);  // u64 all ones
  }
  put32(p, 0);
  put32(p, 3850070400U + 3661U);
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  CHECK(fb.datetime1904("creation_time", false) == "1904-01-01T00:00:00Z");
  CHECK(fb.datetime1904("t", false) == "1970-01-01T00:00:00Z");
  CHECK(fb.datetime1904("t", false) == "2000-02-29T00:00:00Z");
  CHECK(fb.datetime1904("t", true) == "out of range");
  CHECK(fb.datetime1904("t", true) == "2026-01-01T01:01:01Z");
  CHECK(field(node, 0).svalue == "1904-01-01T00:00:00Z");
}

TEST_CASE("FieldBuilder duration", "[container][fieldbuilder]") {
  Bytes p;
  put32(p, 12345);
  put32(p, 0xFFFFFFFFU);
  put32(p, 7);
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  CHECK(fb.duration("duration", false, 1000) == 12345);
  CHECK(field(node, 0).svalue == "12.345 s");
  fb.duration("duration", false, 1000);
  CHECK(field(node, 1).svalue == "unknown");
  fb.duration("duration", false, 0);
  CHECK(field(node, 2).svalue == "unknown timescale");
}

TEST_CASE("FieldBuilder matrix", "[container][fieldbuilder]") {
  const auto words = [](std::array<uint32_t, 9> w) {
    Bytes b;
    for (uint32_t v : w) {
      put32(b, v);
    }
    return b;
  };
  constexpr uint32_t kOne = 0x10000;
  constexpr uint32_t kW = 0x40000000;
  const auto derived_of = [&](const Bytes& b) {
    ovt::BitReader br(b);
    auto node = make_node();
    iso::FieldBuilder fb(node, 100, br, 0);
    fb.matrix();
    REQUIRE(node.fields.size() == 1);
    CHECK(node.fields[0]->name == "matrix");
    CHECK(node.fields[0]->children.size() == 9);
    return node.fields[0]->svalue;
  };
  CHECK(derived_of(words({kOne, 0, 0, 0, kOne, 0, 0, 0, kW})) == "identity");
  CHECK(derived_of(words({0, kOne, 0, 0xFFFF0000U, 0, 0, 0, 0, kW})) == "rotation 90°");
  CHECK(derived_of(words({0xFFFF0000U, 0, 0, 0, 0xFFFF0000U, 0, 0, 0, kW})) == "rotation 180°");
  CHECK(derived_of(words({0, 0xFFFF0000U, 0, kOne, 0, 0, 0, 0, kW})) == "rotation 270°");
  CHECK(derived_of(words({2 * kOne, 0, 0, 0, kOne, 0, 0, 0, kW})) == "custom");
  CHECK(derived_of(words({2 * kOne, 0, 0, 0, kOne, 0, 0, 0, kW})) == "custom");
}

TEST_CASE("FieldBuilder groups, arrays and the large-array policy", "[container][fieldbuilder]") {
  SECTION("materialise()") {
    const Bytes none;
    ovt::BitReader br(none);
    auto node = make_node();
    iso::FieldBuilder fb(node, 100, br, 0);
    CHECK(iso::FieldBuilder::materialise(500, 1024));
    CHECK_FALSE(iso::FieldBuilder::materialise(500, 1025));
    CHECK(iso::FieldBuilder::materialise(63, 2000));
    CHECK_FALSE(iso::FieldBuilder::materialise(64, 2000));
    CHECK_FALSE(iso::FieldBuilder::materialise(1983, 2000));
    CHECK(iso::FieldBuilder::materialise(1984, 2000));
  }
  SECTION("a 2000-element array has 80 element children") {
    Bytes p;
    for (uint32_t i = 0; i < 2000; ++i) {
      put32(p, i);
    }
    ovt::BitReader br(p);
    auto node = make_node();
    iso::FieldBuilder fb(node, 100, br);
    fb.array("entries", 2000);
    for (uint32_t i = 0; i < 2000; ++i) {
      if (iso::FieldBuilder::materialise(i, 2000)) {
        fb.array_element("entry", i);
        fb.u32("sample_delta");
        fb.end();
      } else {
        fb.reader().skip_bytes(4);
      }
    }
    fb.end();
    REQUIRE(node.fields.size() == 1);
    const auto& arr = *node.fields[0];
    CHECK(arr.name == "entries");
    CHECK(arr.uvalue == 2000);
    CHECK(arr.svalue == "see Sample Table view");
    CHECK(arr.children.size() == 80);
    CHECK(arr.children.back()->name == "entry[1999]");
    CHECK(arr.children.back()->children.at(0)->uvalue == 1999);
    CHECK(arr.span->bit_length() == uint64_t{2000} * 32U);
    CHECK(arr.children.back()->span->bit_offset() == kHeaderBits + uint64_t{1999} * 32U);
  }
}

TEST_CASE("FieldBuilder annotations and diagnostics", "[container][fieldbuilder]") {
  const Bytes p = {0, 0, 0, 16, 0, 0, 0, 2, 'e', 'n', 'g', 0};
  ovt::BitReader br(p);
  auto node = make_node();
  iso::FieldBuilder fb(node, 100, br, 0);
  fb.u32("transfer_characteristics");
  fb.enum_name("PQ (SMPTE ST 2084)");
  fb.spec_ref("ISO/IEC 23091-2 §8.2");
  CHECK(field(node, 0).kind == SyntaxValueKind::Enum);
  CHECK(field(node, 0).svalue == "PQ (SMPTE ST 2084)");
  CHECK(field(node, 0).spec_ref == "ISO/IEC 23091-2 §8.2");
  fb.u32("count");
  fb.derived("two");
  CHECK(field(node, 1).svalue == "two");
  fb.str_nul("s", 8);
  fb.diag(Severity::Warning, "CNT-9999", "something odd");
  REQUIRE(node.diagnostics.size() == 1);
  CHECK(node.diagnostics[0]->severity == ovt::fb::Severity::Warning);
  CHECK(node.diagnostics[0]->code == "CNT-9999");
  CHECK(node.diagnostics[0]->span->bit_offset() == field(node, 2).span->bit_offset());
}

namespace {
uint64_t decode_throws(iso::BoxContext& /*ctx*/, ovt::BitReader& /*br*/, iso::FieldBuilder& fb) {
  fb.u32("first");
  fb.group("inner");
  fb.u16("second");
  fb.u32("third");  // only 2 bytes left: BR-0001
  return 0;
}
}  // namespace

TEST_CASE("A decoder that throws keeps its earlier fields and adds CNT-1002", "[container][fieldbuilder]") {
  const Bytes p = {0, 0, 0, 1, 0, 2, 0, 3};
  ovt::BitReader br(p);
  auto node = make_node("tkhd");
  iso::FieldBuilder fb(node, 100, br);
  iso::BoxContext ctx;
  const iso::BoxDecoder dec{"tkhd", "*", &decode_throws};
  const auto out = iso::run_decoder(dec, ctx, br, fb);
  CHECK(out.truncated);
  REQUIRE(node.fields.size() == 2);
  CHECK(node.fields[0]->name == "first");
  CHECK(node.fields[1]->name == "inner");
  REQUIRE(node.fields[1]->children.size() == 1);
  CHECK(node.fields[1]->children[0]->uvalue == 2);
  CHECK(node.fields[1]->span->bit_length() == 16);  // the group was closed at the throw
  REQUIRE(node.diagnostics.size() == 1);
  const auto& d = *node.diagnostics[0];
  CHECK(d.code == "CNT-1002");
  CHECK(d.severity == ovt::fb::Severity::Error);
  CHECK(d.message.starts_with("truncated tkhd: "));
  CHECK(d.span->bit_offset() == kHeaderBits + 48);  // where the failing read started
}
