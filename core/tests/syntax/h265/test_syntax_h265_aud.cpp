// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the H.265 access_unit_delimiter_rbsp(), end_of_seq_rbsp(), end_of_bitstream_rbsp() and filler_data_rbsp()
// parsers (chapter 7 §7.3.5 row 15, WI-1.112 follow-up, FR-SYN-005). The AUD and filler fixtures are the bytes of
// the conformance streams jctvc_CONFWIN_A_Sony_1 (unit 0) and jctvc_FILLER_A_Sony_1 (units 9, 10 and 14), with the
// expected values taken from the committed trace_headers goldens
// tests/golden/h265/jctvc_{CONFWIN_A_Sony_1,FILLER_A_Sony_1}.trace_headers.json.gz (FFmpeg CBS, the oracle):
// `pic_type` 0 / 1 / 2 at bit 16, twenty `ff_byte` elements at bits 16..168. Corpus-wide parity is WI-1.572.

#include "ovt/cxx/syntax/h265/aud.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "h265_test_util.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::BitSpan;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
using ovt::test::h265::parse_nal;
namespace h265 = ovt::syntax::h265;

std::vector<uint8_t> from_hex(std::string_view hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

const SyntaxNodeData* find_node(const SyntaxNodeData* n, std::string_view name) {
  if (n == nullptr) {
    return nullptr;
  }
  if (n->name == name) {
    return n;
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    if (const auto* hit = find_node(c, name)) {
      return hit;
    }
  }
  return nullptr;
}

void collect_diags(const SyntaxNodeData* n, std::vector<std::string>& codes) {
  for (const auto* d = n->first_diag; d != nullptr; d = d->next) {
    codes.emplace_back(d->code);
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    collect_diags(c, codes);
  }
}

std::vector<std::string> diag_messages(const SyntaxNodeData* n) {
  std::vector<std::string> out;
  for (const auto* d = n->first_diag; d != nullptr; d = d->next) {
    out.emplace_back(d->message);
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    for (auto& m : diag_messages(c)) {
      out.push_back(std::move(m));
    }
  }
  return out;
}

std::vector<std::string> diag_codes(const SyntaxNodeBuffer& tree) {
  std::vector<std::string> codes;
  if (tree.root() != nullptr) {
    collect_diags(tree.root(), codes);
  }
  return codes;
}

// jctvc_CONFWIN_A_Sony_1 unit 0 (AUD_NUT, pic_type 0) and jctvc_FILLER_A_Sony_1 units 10 / 14 (pic_type 1 / 2).
constexpr std::string_view kAudI = "460110";
constexpr std::string_view kAudPI = "460130";
constexpr std::string_view kAudBPI = "460150";
// jctvc_FILLER_A_Sony_1 unit 9 (FD_NUT): twenty ff_byte then rbsp_trailing_bits().
constexpr std::string_view kFiller = "4c01ffffffffffffffffffffffffffffffffffffffff80";

}  // namespace

TEST_CASE("h265_aud_unit", "[syntax][h265][fast]") {
  SECTION("CONFWIN_A unit 0: pic_type 0 (I)") {
    SyntaxNodeBuffer tree;
    const auto aud = parse_nal(
        from_hex(kAudI),
        [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader& h) {
          REQUIRE(h.nal_unit_type == h265::kAudNut);
          return h265::parse_access_unit_delimiter(br, b);
        },
        &tree);
    CHECK(aud.pic_type == 0);
    CHECK(aud.pic_type_name == "I");
    CHECK(aud.span.bit_offset == 16);
    CHECK(aud.span.bit_length == 8);
    const auto* node = find_node(tree.root(), "pic_type");
    REQUIRE(node != nullptr);
    CHECK(node->span.bit_offset == 16);  // trace_headers: pic_type at bit 16, length 3
    CHECK(node->span.bit_length == 3);
    CHECK(node->enum_name == "I");
    CHECK(find_node(tree.root(), "access_unit_delimiter_rbsp") != nullptr);
    CHECK(find_node(tree.root(), "rbsp_stop_one_bit") != nullptr);
    CHECK(diag_codes(tree).empty());
    CHECK(ovt::syntax::to_trace(tree, from_hex(kAudI)) ==
          "0 forbidden_zero_bit 0 = 0\n1 nal_unit_type 100011 = 35\n7 nuh_layer_id 000000 = 0\n"
          "13 nuh_temporal_id_plus1 001 = 1\n16 pic_type 000 = 0\n19 rbsp_stop_one_bit 1 = 1\n"
          "20 rbsp_alignment_zero_bit 0 = 0\n21 rbsp_alignment_zero_bit 0 = 0\n22 rbsp_alignment_zero_bit 0 = 0\n"
          "23 rbsp_alignment_zero_bit 0 = 0\n");
  }
  SECTION("FILLER_A units 10 and 14: pic_type 1 (P, I) and 2 (B, P, I)") {
    const auto p = parse_nal(from_hex(kAudPI), [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) {
      return h265::parse_access_unit_delimiter(br, b);
    });
    CHECK(p.pic_type == 1);
    CHECK(p.pic_type_name == "P, I");
    const auto bpi = parse_nal(from_hex(kAudBPI), [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) {
      return h265::parse_access_unit_delimiter(br, b);
    });
    CHECK(bpi.pic_type == 2);
    CHECK(bpi.pic_type_name == "B, P, I");
  }
  SECTION("reserved pic_type 5 is Diagnostic Error SYN-H265-0007 and parsing continues") {
    SyntaxNodeBuffer tree;
    const auto aud = parse_nal(
        from_hex("4601b0"),
        [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) {
          return h265::parse_access_unit_delimiter(br, b);
        },
        &tree);
    CHECK(aud.pic_type == 5);
    CHECK(aud.pic_type_name.empty());
    CHECK(h265::pic_type_name(7).empty());
    CHECK(diag_codes(tree) == std::vector<std::string>{"SYN-H265-0007"});
    CHECK(find_node(tree.root(), "rbsp_stop_one_bit") != nullptr);
  }
  SECTION("truncated AUD (NAL unit header only) is a ParseError") {
    CHECK_THROWS_AS(
        parse_nal(from_hex("4601"), [](BitReader& br, SyntaxBuilder& b,
                                       const h265::NalHeader&) { return h265::parse_access_unit_delimiter(br, b); }),
        ovt::ParseError);
  }
}

TEST_CASE("h265_eos_eob_unit", "[syntax][h265][fast]") {
  SECTION("empty RBSPs") {
    SyntaxNodeBuffer eos_tree;
    const BitSpan eos = parse_nal(
        from_hex("4801"),
        [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader& h) {
          REQUIRE(h.nal_unit_type == h265::kEosNut);
          return h265::parse_end_of_seq(br, b);
        },
        &eos_tree);
    CHECK(eos.bit_offset == 16);
    CHECK(eos.bit_length == 0);
    CHECK(find_node(eos_tree.root(), "end_of_seq_rbsp") != nullptr);
    CHECK(diag_codes(eos_tree).empty());
    SyntaxNodeBuffer eob_tree;
    const BitSpan eob = parse_nal(
        from_hex("4a01"),
        [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader& h) {
          REQUIRE(h.nal_unit_type == h265::kEobNut);
          return h265::parse_end_of_bitstream(br, b);
        },
        &eob_tree);
    CHECK(eob.bit_offset == 16);
    CHECK(eob.bit_length == 0);
    CHECK(find_node(eob_tree.root(), "end_of_bitstream_rbsp") != nullptr);
    CHECK(diag_codes(eob_tree).empty());
  }
  SECTION("bytes after the header: Info SYN-H265-0033 with the byte count, and the opaque trailing_data node") {
    SyntaxNodeBuffer tree;
    const BitSpan eos = parse_nal(
        from_hex("48011234"),
        [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) { return h265::parse_end_of_seq(br, b); }, &tree);
    CHECK(eos.bit_length == 16);
    CHECK(diag_codes(tree) == std::vector<std::string>{"SYN-H265-0033"});
    CHECK(diag_messages(tree.root()) ==
          std::vector<std::string>{"end_of_seq_rbsp has 2 trailing bytes after the NAL unit header"});
    const auto* junk = find_node(tree.root(), "trailing_data");
    REQUIRE(junk != nullptr);
    CHECK(junk->span.bit_offset == 16);
    CHECK(junk->span.bit_length == 16);
    SyntaxNodeBuffer eob_tree;
    (void)parse_nal(
        from_hex("4a01ff"),
        [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) { return h265::parse_end_of_bitstream(br, b); },
        &eob_tree);
    CHECK(diag_codes(eob_tree) == std::vector<std::string>{"SYN-H265-0033"});
    CHECK(diag_messages(eob_tree.root()) ==
          std::vector<std::string>{"end_of_bitstream_rbsp has 1 trailing bytes after the NAL unit header"});
  }
}

TEST_CASE("h265_filler_data_unit", "[syntax][h265][fast]") {
  SECTION("FILLER_A unit 9: the twenty ff_byte as one node") {
    SyntaxNodeBuffer tree;
    const auto f = parse_nal(
        from_hex(kFiller),
        [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader& h) {
          REQUIRE(h.nal_unit_type == h265::kFdNut);
          return h265::parse_filler_data(br, b);
        },
        &tree);
    CHECK(f.ff_byte_count == 20);
    CHECK(f.span.bit_offset == 16);
    CHECK(f.span.bit_length == 168);
    const auto* ff = find_node(tree.root(), "ff_byte");
    REQUIRE(ff != nullptr);
    CHECK(ff->descriptor == "f(8)*20");
    CHECK(ff->span.bit_offset == 16);  // trace_headers: ff_byte at bits 16, 24, ..., 168 (20 elements)
    CHECK(ff->span.bit_length == 160);
    CHECK(ff->bytes.size() == 20);
    CHECK(find_node(tree.root(), "ff_byte_count") != nullptr);
    CHECK(find_node(tree.root(), "rbsp_stop_one_bit")->span.bit_offset == 176);
    CHECK(diag_codes(tree).empty());
  }
  SECTION("no ff_byte: only rbsp_trailing_bits()") {
    SyntaxNodeBuffer tree;
    const auto f = parse_nal(
        from_hex("4c0180"),
        [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) { return h265::parse_filler_data(br, b); }, &tree);
    CHECK(f.ff_byte_count == 0);
    CHECK(find_node(tree.root(), "ff_byte") == nullptr);
    CHECK(diag_codes(tree).empty());
  }
  SECTION("missing rbsp_trailing_bits() after the run is a ParseError, not a crash") {
    CHECK_THROWS_AS(
        parse_nal(from_hex("4c01ffff"), [](BitReader& br, SyntaxBuilder& b,
                                           const h265::NalHeader&) { return h265::parse_filler_data(br, b); }),
        ovt::ParseError);
  }
  SECTION("deterministic: two parses give the same JSON (§0.7.5)") {
    SyntaxNodeBuffer first;
    SyntaxNodeBuffer again;
    const auto parse = [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) {
      return h265::parse_filler_data(br, b);
    };
    (void)parse_nal(from_hex(kFiller), parse, &first);
    (void)parse_nal(from_hex(kFiller), parse, &again);
    CHECK(ovt::syntax::to_json(first) == ovt::syntax::to_json(again));
  }
}
