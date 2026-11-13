// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// SyntaxBuilder and SyntaxNodeBuffer unit tests (chapter 7 §7.3.2, WI-1.106).

#include "ovt/cxx/syntax/syntax_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::ParseError;
using ovt::syntax::DiagnosticData;
using ovt::syntax::Severity;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
using ovt::syntax::ValueKind;

bool span_is(const ovt::BitSpan& s, uint64_t off, uint32_t len) {
  return s.bit_offset == off && s.bit_length == len;
}

// Values a toy parser returns; compared between recording on and off.
struct ToyValues {
  uint64_t a = 0;
  bool flag = false;
  uint32_t ue = 0;
  int32_t se = 0;
  std::vector<int64_t> arr;
  uint64_t le = 0;
  uint64_t end_pos = 0;
};

// A small structure with nesting, an array, derived values and an enum, read the way a parser would.
ToyValues parse_toy(BitReader& br, SyntaxBuilder& b) {
  ToyValues v;
  auto s = b.scoped("toy_rbsp", "TOY 1.2.3");
  v.a = b.read_u("a", 5);
  b.set_enum("FIVE_BITS");
  v.flag = b.read_flag("flag");
  v.ue = b.read_ue("ue_elem");
  v.se = b.read_se("se_elem");
  b.derived("TwiceUe", 2 * v.ue);
  {
    auto inner = b.scoped("inner");
    for (int i = 0; i < 3; ++i) {
      b.begin_array_element("arr", i);
      v.arr.push_back(b.read_i("arr", 4));
      b.end_array_element();
    }
  }
  while (!br.byte_aligned()) {
    b.read_f("alignment_bit", 1, 0);
  }
  v.le = b.read_le("le_elem", 2);
  v.end_pos = br.bit_pos();
  return v;
}

// a=10110 flag=1 ue=00100 (3) se=011 (-1) arr: 1111 (-1) 0111 (7) 1000 (-8), six alignment zeros (26 + 6 = 32
// bits), then le(2) of 12 34 (0x3412).
std::vector<uint8_t> toy_bytes() {
  const std::string bits =
      "10110"
      "1"
      "00100"
      "011"
      "1111"
      "0111"
      "1000"
      "000000";
  std::vector<uint8_t> out(bits.size() / 8, 0);
  for (size_t i = 0; i < bits.size(); ++i) {
    if (bits[i] == '1') {
      out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80U >> (i % 8)));
    }
  }
  out.push_back(0x12);
  out.push_back(0x34);
  return out;
}

struct RecordingSink final : BitReader::DiagnosticSink {
  int calls = 0;
  void reader_warning(std::string_view /*code*/, std::string_view /*message*/, uint64_t /*bit_offset*/) override {
    ++calls;
  }
};

}  // namespace

// ---- acceptance tests ---------------------------------------------------------------------------------------

TEST_CASE("syntax_builder_struct_spans: three u(3) reads nest in the struct span", "[syntax][fast]") {
  const std::array<uint8_t, 2> data = {0b101'010'11, 0b1'0000000};
  BitReader br(data);
  SyntaxBuilder b(br);
  b.begin_struct("toy");
  CHECK(b.read_u("x", 3) == 5);
  CHECK(b.read_u("y", 3) == 2);
  CHECK(b.read_u("z", 3) == 7);
  b.end_struct();
  const SyntaxNodeBuffer buf = b.finish();
  REQUIRE(buf.root() != nullptr);
  REQUIRE(buf.root()->child_count() == 1);
  const SyntaxNodeData* s = buf.root()->child(0);
  CHECK(s->name == "toy");
  CHECK(s->kind == ValueKind::None);
  CHECK(span_is(s->span, 0, 9));
  REQUIRE(s->child_count() == 3);
  CHECK(span_is(s->child(0)->span, 0, 3));
  CHECK(span_is(s->child(1)->span, 3, 3));
  CHECK(span_is(s->child(2)->span, 6, 3));
  for (size_t i = 0; i < 3; ++i) {
    CHECK(s->child(i)->descriptor == "u(3)");
    CHECK(s->child(i)->kind == ValueKind::UInt);
    CHECK(s->child(i)->array_index == -1);
  }
  CHECK(s->child(0)->name == "x");
  CHECK(s->child(2)->uvalue == 7);
  CHECK(span_is(buf.root()->span, 0, 9));
}

TEST_CASE("syntax_builder_recording_off: 1 M reads allocate nothing and return the same values", "[syntax][fast]") {
  constexpr size_t kElements = 1'000'000;
  constexpr unsigned kWidth = 7;
  std::vector<uint8_t> data((kElements * kWidth + 7) / 8);
  std::mt19937 rng(1106);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seed, reproducible test data
  for (auto& byte : data) {
    byte = static_cast<uint8_t>(rng());
  }

  std::vector<uint64_t> expected(kElements);
  {
    BitReader ref(data);
    for (auto& e : expected) {
      e = ref.u(kWidth);
    }
  }

  BitReader br(data);
  SyntaxBuilder b(br);
  b.set_recording(false);
  bool same = true;
  {
    auto s = b.scoped("payload");
    for (size_t i = 0; i < kElements; ++i) {
      b.begin_array_element("element", static_cast<int>(i));
      same = same && b.read_u("element", kWidth) == expected[i];
      b.set_enum("ignored");
      b.derived("ignored", 1);
      b.end_array_element();
    }
  }
  CHECK(same);
  const SyntaxNodeBuffer buf = b.finish();
  CHECK(buf.empty());
  CHECK(buf.arena_bytes() == 0);
  CHECK(buf.node_count() == 0);
}

TEST_CASE("syntax_builder_partial_tree: finish() after a ParseError keeps the nodes read before it", "[syntax][fast]") {
  const std::array<uint8_t, 1> data = {0xFF};
  BitReader br(data);
  SyntaxBuilder b(br, "TOY 7.3");
  bool threw = false;
  try {
    auto s = b.scoped("toy");
    (void)b.read_u("first", 3);
    (void)b.read_u("second", 3);
    (void)b.read_u("third", 3);  // 6 + 3 > 8 bits: BR-0001
  } catch (const ParseError& e) {
    threw = true;
    CHECK(e.code == "BR-0001");
  }
  CHECK(threw);
  const SyntaxNodeBuffer buf = b.finish();
  REQUIRE(buf.root()->child_count() == 1);
  const SyntaxNodeData* s = buf.root()->child(0);
  REQUIRE(s->child_count() == 2);
  CHECK(s->child(0)->name == "first");
  CHECK(s->child(1)->name == "second");
  CHECK(span_is(s->span, 0, 6));
  CHECK(buf.node_count() == 3);
}

TEST_CASE("syntax_builder_partial_tree: structs left open by a throw are closed by finish()", "[syntax][fast]") {
  const std::array<uint8_t, 1> data = {0xFF};
  BitReader br(data);
  SyntaxBuilder b(br);
  b.begin_struct("outer");
  b.begin_struct("inner");
  (void)b.read_u("v", 4);
  CHECK_THROWS_AS(b.read_u("w", 8), ParseError);
  const SyntaxNodeBuffer buf = b.finish();
  const SyntaxNodeData* outer = buf.root()->child(0);
  REQUIRE(outer != nullptr);
  CHECK(span_is(outer->span, 0, 4));
  CHECK(span_is(outer->child(0)->span, 0, 4));
  CHECK(outer->child(0)->child_count() == 1);
}

TEST_CASE("syntax_builder_sink: reader warnings become diagnostics and the previous sink is restored",
          "[syntax][fast]") {
  // leb128 whose 8th byte still has its continuation bit: BR-0009 Warning, not thrown (§7.2.4).
  const std::array<uint8_t, 8> data = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
  BitReader br(data);
  RecordingSink s;
  br.set_diagnostic_sink(&s);
  {
    SyntaxBuilder b(br, "AV1 5.3.1");
    CHECK(br.diagnostic_sink() == &b);
    auto scope = b.scoped("obu_header_ext");
    CHECK(b.read_leb128("obu_size") == 0);
    const SyntaxNodeBuffer buf = b.finish();
    const SyntaxNodeData* st = buf.root()->child(0);
    REQUIRE(st->diagnostic_count() == 1);
    const DiagnosticData* d = st->first_diag;
    CHECK(d->severity == Severity::Warning);
    CHECK(d->code == "BR-0009");
    CHECK(d->span.bit_length == 0);
    CHECK(d->spec_ref == "AV1 5.3.1");
    CHECK(st->child(0)->diagnostic_count() == 0);
    CHECK(s.calls == 0);
  }
  CHECK(br.diagnostic_sink() == &s);
}

// ---- other behaviour ----------------------------------------------------------------------------------------

TEST_CASE("syntax_builder: a toy structure gives identical values with recording on and off", "[syntax][fast]") {
  const std::vector<uint8_t> data = toy_bytes();
  BitReader br_on(data);
  SyntaxBuilder on(br_on);
  const ToyValues v_on = parse_toy(br_on, on);
  BitReader br_off(data);
  SyntaxBuilder off(br_off);
  off.set_recording(false);
  const ToyValues v_off = parse_toy(br_off, off);

  CHECK(v_on.a == 0b10110);
  CHECK(v_on.flag);
  CHECK(v_on.ue == 3);
  CHECK(v_on.se == -1);
  CHECK(v_on.arr == std::vector<int64_t>{-1, 7, -8});
  CHECK(v_on.le == 0x3412);
  CHECK(v_on.end_pos == 48);
  CHECK(v_on.a == v_off.a);
  CHECK(v_on.flag == v_off.flag);
  CHECK(v_on.ue == v_off.ue);
  CHECK(v_on.se == v_off.se);
  CHECK(v_on.arr == v_off.arr);
  CHECK(v_on.le == v_off.le);
  CHECK(v_on.end_pos == v_off.end_pos);
  CHECK(off.finish().empty());

  const SyntaxNodeBuffer buf = on.finish();
  const SyntaxNodeData* toy = buf.root()->child(0);
  CHECK(toy->spec_ref == "TOY 1.2.3");
  CHECK(span_is(toy->span, 0, 48));
  CHECK(toy->child(0)->enum_name == "FIVE_BITS");
  CHECK(toy->child(0)->spec_ref == "TOY 1.2.3");  // leaves name the table of their struct (FR-SYN-007)
  CHECK(toy->child(1)->kind == ValueKind::Bool);
  CHECK(toy->child(1)->descriptor == "u(1)");
  CHECK(toy->child(2)->descriptor == "ue(v)");
  CHECK(span_is(toy->child(2)->span, 6, 5));
  CHECK(toy->child(3)->descriptor == "se(v)");
  CHECK(toy->child(3)->ivalue == -1);
  const SyntaxNodeData* derived = toy->child(4);
  CHECK(derived->name == "TwiceUe");
  CHECK(derived->kind == ValueKind::None);
  CHECK(derived->derived == "TwiceUe = 6");
  CHECK(span_is(derived->span, 14, 0));
  const SyntaxNodeData* inner = toy->child(5);
  CHECK(inner->name == "inner");
  CHECK(span_is(inner->span, 14, 12));
  REQUIRE(inner->child_count() == 3);
  for (size_t i = 0; i < 3; ++i) {
    const SyntaxNodeData* el = inner->child(i);
    CHECK(el->array_index == static_cast<int>(i));
    CHECK(el->child(0)->array_index == static_cast<int>(i));
    CHECK(el->child(0)->descriptor == "i(4)");
    CHECK(span_is(el->span, 14 + 4 * i, 4));
  }
  CHECK(toy->child(6)->descriptor == "f(1)");
  CHECK(toy->child(12)->descriptor == "le(2)");
  CHECK(toy->diagnostic_count() == 0);
}

TEST_CASE("syntax_builder: read_f mismatch is an Error diagnostic on the node and parsing continues",
          "[syntax][fast]") {
  const std::array<uint8_t, 1> data = {0b0110'0000};
  BitReader br(data);
  SyntaxBuilder b(br);
  auto s = b.scoped("s");
  CHECK(b.read_f("forbidden_zero_bit", 1, 0) == 0);
  CHECK(b.read_f("marker", 2, 0) == 3);
  CHECK(b.read_u("next", 1) == 0);
  const SyntaxNodeBuffer buf = b.finish();
  const SyntaxNodeData* st = buf.root()->child(0);
  CHECK(st->child(0)->diagnostic_count() == 0);
  const SyntaxNodeData* marker = st->child(1);
  REQUIRE(marker->diagnostic_count() == 1);
  CHECK(marker->first_diag->severity == Severity::Error);
  CHECK(marker->first_diag->code == "SYN-GEN-0001");
  CHECK(marker->first_diag->message == "marker: fixed-pattern f(2) read 3, expected 0");
  CHECK(span_is(marker->first_diag->span, 1, 2));
  CHECK(st->child_count() == 3);
}

TEST_CASE("syntax_builder: diagnostics survive with recording off", "[syntax][fast]") {
  const std::array<uint8_t, 1> data = {0xFF};
  BitReader br(data);
  SyntaxBuilder b(br, "TOY 1");
  b.set_recording(false);
  auto s = b.scoped("s");
  (void)b.read_f("must_be_zero", 1, 0);
  b.diag(Severity::Warning, "SYN-TOY-0001", "reserved value", {}, true);
  const SyntaxNodeBuffer buf = b.finish();
  REQUIRE_FALSE(buf.empty());
  CHECK(buf.node_count() == 0);
  REQUIRE(buf.root()->diagnostic_count() == 2);
  CHECK(buf.root()->first_diag->code == "SYN-GEN-0001");
  CHECK(buf.root()->last_diag->code == "SYN-TOY-0001");
  CHECK(buf.root()->last_diag->spec_ref == "TOY 1");
}

TEST_CASE("syntax_builder: descriptor text, enum and diag decorate the last node", "[syntax][fast]") {
  const std::array<uint8_t, 4> data = {0xA5, 0x5A, 0x00, 0x80};
  BitReader br(data);
  SyntaxBuilder b(br);
  auto s = b.scoped("slice_header", "H.264 7.3.3");
  (void)b.read_u("slice_pic_order_cnt_lsb", 6);
  b.set_descriptor_text("u(v)");
  (void)b.read_ue("slice_type");
  b.set_enum("P");
  b.diag(Severity::Warning, "SYN-H264-0099", "on the last node", "H.264 7.4.3", true);
  b.diag(Severity::Info, "SYN-H264-0098", "on the struct");
  const SyntaxNodeBuffer buf = b.finish();
  const SyntaxNodeData* st = buf.root()->child(0);
  CHECK(st->child(0)->descriptor == "u(v)");
  CHECK(st->child(1)->enum_name == "P");
  REQUIRE(st->child(1)->diagnostic_count() == 1);
  CHECK(st->child(1)->first_diag->spec_ref == "H.264 7.4.3");
  CHECK(span_is(st->child(1)->first_diag->span, st->child(1)->span.bit_offset, st->child(1)->span.bit_length));
  REQUIRE(st->diagnostic_count() == 1);
  CHECK(st->first_diag->spec_ref == "H.264 7.3.3");
  CHECK(st->first_diag->span.bit_length == 0);
}

TEST_CASE("syntax_builder: AV1 reads, read_delta_q children, bytes and opaque_rest", "[syntax][fast]") {
  // uvlc 010 (1) | su(4) 1000 (-8) | ns(5) 110 (3) | delta_q 1 1000000 (-64) | delta_q 0 | five zero bits
  //   = 01010001 10110000 00000000, then (byte aligned): bytes "ab" | leb128 05 | st "hi\0" | 3 opaque bytes
  const std::vector<uint8_t> data = {0b0101'0001, 0b1011'0000, 0b0000'0000, 'a', 'b', 0x05, 'h', 'i', 0, 1, 2, 3};
  BitReader br(data);
  SyntaxBuilder b(br, "AV1 5.9");
  auto s = b.scoped("toy_obu");
  CHECK(b.read_uvlc("u") == 1);
  CHECK(b.read_su("s", 4) == -8);
  CHECK(b.read_ns("n", 5) == 3);
  CHECK(b.read_delta_q("DeltaQYDc") == -64);
  CHECK(b.read_delta_q("DeltaQUDc") == 0);
  while (!br.byte_aligned()) {
    (void)b.read_f("zero_bit", 1, 0);
  }
  const auto bytes = b.read_bytes("two_bytes", 2);
  CHECK(bytes.size() == 2);
  CHECK(bytes[0] == 'a');
  CHECK(b.read_leb128("obu_size") == 5);
  CHECK(b.read_st("name") == "hi");
  b.opaque_rest("tile_data");
  CHECK(br.bits_left() == 0);
  const SyntaxNodeBuffer buf = b.finish();
  const SyntaxNodeData* st = buf.root()->child(0);
  CHECK(st->child(0)->descriptor == "uvlc()");
  CHECK(st->child(1)->descriptor == "su(4)");
  CHECK(st->child(2)->descriptor == "ns(5)");
  const SyntaxNodeData* dq = st->child(3);
  CHECK(dq->kind == ValueKind::Int);
  CHECK(dq->ivalue == -64);
  CHECK(span_is(dq->span, 10, 8));
  REQUIRE(dq->child_count() == 2);
  CHECK(dq->child(0)->name == "delta_coded");
  CHECK(dq->child(0)->descriptor == "f(1)");
  CHECK(dq->child(1)->name == "delta_q");
  CHECK(dq->child(1)->descriptor == "su(1+6)");
  CHECK(span_is(dq->child(1)->span, 11, 7));
  const SyntaxNodeData* dq2 = st->child(4);
  CHECK(dq2->child_count() == 1);
  CHECK(span_is(dq2->span, 18, 1));
  const SyntaxNodeData* last = st->last_child;
  CHECK(last->name == "tile_data");
  CHECK(last->kind == ValueKind::Bytes);
  CHECK(last->bytes.empty());
  CHECK(span_is(last->span, 72, 24));
  const SyntaxNodeData* two = st->child(st->child_count() - 4);
  CHECK(two->name == "two_bytes");
  CHECK(two->descriptor == "b(8)*2");
  CHECK(two->bytes.size() == 2);
  CHECK(st->child(st->child_count() - 2)->svalue == "hi");
  CHECK(st->child(st->child_count() - 2)->descriptor == "st(v)");
}

TEST_CASE("syntax_builder: read_bytes keeps only the span above 256 bytes", "[syntax][fast]") {
  const std::vector<uint8_t> data(300, 0xAB);
  BitReader br(data);
  SyntaxBuilder b(br);
  CHECK(b.read_bytes("big", 257).size() == 257);
  CHECK(b.read_bytes("small", 43).size() == 43);
  const SyntaxNodeBuffer buf = b.finish();
  CHECK(buf.root()->child(0)->bytes.empty());
  CHECK(span_is(buf.root()->child(0)->span, 0, 257 * 8));
  CHECK(buf.root()->child(1)->bytes.size() == 43);
}

TEST_CASE("syntax_builder: FlatBuffer conversion verifies and is deterministic", "[syntax][fast]") {
  const std::vector<uint8_t> data = toy_bytes();
  const auto build = [&] {
    BitReader br(data);
    SyntaxBuilder b(br, "TOY 1");
    (void)parse_toy(br, b);
    b.diag(Severity::Error, "SYN-TOY-0002", "unit diag");
    SyntaxNodeBuffer buf = b.finish();
    buf.set_root_name("toy_unit");
    return buf.to_flatbuffer();
  };
  const std::vector<uint8_t> fb1 = build();
  const std::vector<uint8_t> fb2 = build();
  CHECK(fb1 == fb2);
  flatbuffers::Verifier v(fb1.data(), fb1.size());
  REQUIRE(v.VerifyBuffer<ovt::fb::SyntaxNode>(nullptr));
  const auto* root = flatbuffers::GetRoot<ovt::fb::SyntaxNode>(fb1.data());
  CHECK(root->name()->string_view() == "toy_unit");
  CHECK(root->spec_ref()->string_view() == "TOY 1");
  REQUIRE(root->children()->size() == 1);
  REQUIRE(root->diagnostics()->size() == 1);
  CHECK(root->diagnostics()->Get(0)->severity() == ovt::fb::Severity::Error);
  const auto* toy = root->children()->Get(0);
  CHECK(toy->name()->string_view() == "toy_rbsp");
  CHECK(toy->span()->bit_length() == 48);
  const auto* a = toy->children()->Get(0);
  CHECK(a->kind() == ovt::fb::SyntaxValueKind::UInt);
  CHECK(a->uvalue() == 0b10110);
  CHECK(a->descriptor()->string_view() == "u(5)");
  CHECK(a->enum_name()->string_view() == "FIVE_BITS");
  CHECK(a->array_index() == -1);
  const auto* el = toy->children()->Get(5)->children()->Get(2);
  CHECK(el->array_index() == 2);
  CHECK(el->children()->Get(0)->ivalue() == -8);
  CHECK(SyntaxNodeBuffer{}.to_flatbuffer().empty());
}

TEST_CASE("syntax_dump: JSON uses the FlatBuffer field names", "[syntax][fast]") {
  const std::array<uint8_t, 2> data = {0b1100'0000, 0x00};
  BitReader br(data);
  SyntaxBuilder b(br, "T 1");
  {
    auto s = b.scoped("s");
    (void)b.read_u("x", 2);
    b.set_enum("THREE");
    b.begin_array_element("e", 0);
    (void)b.read_flag("e");
    b.end_array_element();
    b.derived("X2", "X2 = 6");
    b.diag(Severity::Warning, "SYN-T-0001", "say \"hi\"\n");
  }
  const std::string json = to_json(b.finish());
  const std::string expected = R"json({
  "name": "unit",
  "span": {"bit_offset": 0, "bit_length": 3},
  "spec_ref": "T 1",
  "children": [
    {
      "name": "s",
      "span": {"bit_offset": 0, "bit_length": 3},
      "spec_ref": "T 1",
      "children": [
        {
          "name": "x",
          "descriptor": "u(2)",
          "kind": "UInt",
          "uvalue": 3,
          "enum_name": "THREE",
          "span": {"bit_offset": 0, "bit_length": 2},
          "spec_ref": "T 1"
        },
        {
          "name": "e",
          "span": {"bit_offset": 2, "bit_length": 1},
          "spec_ref": "T 1",
          "children": [
            {
              "name": "e",
              "descriptor": "u(1)",
              "kind": "Bool",
              "bvalue": false,
              "span": {"bit_offset": 2, "bit_length": 1},
              "spec_ref": "T 1",
              "array_index": 0
            }
          ],
          "array_index": 0
        },
        {
          "name": "X2",
          "derived": "X2 = 6",
          "span": {"bit_offset": 3, "bit_length": 0},
          "spec_ref": "T 1"
        }
      ],
      "diagnostics": [
        {
          "severity": "Warning",
          "code": "SYN-T-0001",
          "message": "say \"hi\"\n",
          "span": {"bit_offset": 3, "bit_length": 0},
          "spec_ref": "T 1"
        }
      ]
    }
  ]
}
)json";
  CHECK(json == expected);
  CHECK(to_json(SyntaxNodeBuffer{}) == "null\n");
}

TEST_CASE("syntax_dump: JSON strings from the stream are escaped and kept valid UTF-8", "[syntax][fast]") {
  const std::vector<uint8_t> data = {'a', 0x01, 0xC3, 0xA9, 0xFF, '\\', 0};
  BitReader br(data);
  SyntaxBuilder b(br);
  (void)b.read_st("str");
  const std::string json = to_json(b.finish());
  CHECK(json.find("\"svalue\": \"a\\u0001\xC3\xA9\\ufffd\\\\\"") != std::string::npos);
}

TEST_CASE("syntax_dump: trace lines follow the trace_headers layout", "[syntax][fast]") {
  const std::vector<uint8_t> data = toy_bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  (void)parse_toy(br, b);
  const std::string trace = to_trace(b.finish(), data);
  const std::string expected =
      "0 a 10110 = 22\n"
      "5 flag 1 = 1\n"
      "6 ue_elem 00100 = 3\n"
      "11 se_elem 011 = -1\n"
      "14 arr[0] 1111 = -1\n"
      "18 arr[1] 0111 = 7\n"
      "22 arr[2] 1000 = -8\n"
      "26 alignment_bit 0 = 0\n"
      "27 alignment_bit 0 = 0\n"
      "28 alignment_bit 0 = 0\n"
      "29 alignment_bit 0 = 0\n"
      "30 alignment_bit 0 = 0\n"
      "31 alignment_bit 0 = 0\n"
      "32 le_elem 0001001000110100 = 13330\n";
  CHECK(trace == expected);
}

TEST_CASE("syntax_dump: UTF-8 boundaries, \\r and \\t in JSON and trace strings", "[syntax][fast]") {
  // RFC 3629 limits: the first valid and first invalid second byte after E0, ED, F0 and F4, a bad third byte, and a
  // sequence cut off by the end of the string. Each byte of a rejected sequence becomes U+FFFD on its own.
  const std::vector<uint8_t> data = {'x',  '\r', '\t',                                // escapes
                                     0xE0, 0xA0, 0x80, 0xE0, 0x80, 0x80,              // E0: A0 ok, 80 overlong
                                     0xED, 0x9F, 0xBF, 0xED, 0xA0, 0x80,              // ED: 9F ok, A0 surrogate
                                     0xF0, 0x90, 0x80, 0x80, 0xF0, 0x8F, 0x80, 0x80,  // F0: 90 ok, 8F overlong
                                     0xF4, 0x8F, 0xBF, 0xBF, 0xF4, 0x90, 0x80, 0x80,  // F4: 8F ok, 90 > U+10FFFF
                                     0xE2, 0x82, 'A',                                 // bad third byte
                                     0xE2, 0x82,                                      // truncated by the end
                                     0};
  BitReader br(data);
  SyntaxBuilder b(br);
  (void)b.read_st("str");
  const SyntaxNodeBuffer buf = b.finish();
  const std::string ff = "\\ufffd";
  const std::string expected = std::string(R"("x\r\t)") + "\xE0\xA0\x80" + ff + ff + ff + "\xED\x9F\xBF" + ff + ff +
                               ff + "\xF0\x90\x80\x80" + ff + ff + ff + ff + "\xF4\x8F\xBF\xBF" + ff + ff + ff + ff +
                               ff + ff + "A" + ff + ff + "\"";
  CHECK(to_json(buf).find("\"svalue\": " + expected) != std::string::npos);
  // The trace prints the same escaped string; bits beyond the unit bytes show as '?', and only the first 64 bits.
  const std::string trace = to_trace(buf, std::span<const uint8_t>(data.data(), 4));
  CHECK(trace.starts_with("0 str 01111000000011010000100111100000????????????????????????????????... = "));
  CHECK(trace.ends_with(" = " + expected + "\n"));
  CHECK(to_trace(SyntaxNodeBuffer{}, data).empty());
}

TEST_CASE("syntax_builder: read_te records te(v) with the cMax rule", "[syntax][fast]") {
  // te(v) with cMax 1 is one inverted bit; with cMax > 1 it is ue(v) (H.264 §9.1.2). Bits: 0 | 011 | 1.
  const std::vector<uint8_t> data = {0b0011'1000};
  BitReader br(data);
  SyntaxBuilder b(br);
  CHECK(b.read_te("one_bit", 1) == 1);
  CHECK(b.read_te("ue", 7) == 2);
  CHECK(b.read_te("zero", 1) == 0);
  const SyntaxNodeBuffer buf = b.finish();
  const std::string json = to_json(buf);
  CHECK(json.find("\"descriptor\": \"te(v)\"") != std::string::npos);
  CHECK(to_trace(buf, data) == "0 one_bit 0 = 1\n1 ue 011 = 2\n4 zero 1 = 0\n");
}
