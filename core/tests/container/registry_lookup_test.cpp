// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the box decoder registry: context-pattern matching, specificity ordering and registration
// (chapter 08 §8.2.9, WI-1.202).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "container/isobmff/box_registry.h"

namespace {

namespace iso = ovt::container::isobmff;

iso::BoxContext ctx_of(std::initializer_list<std::string_view> chain) {
  iso::BoxContext c;
  for (const std::string_view s : chain) {
    iso::FourCC cc{};
    std::copy_n(s.begin(), 4, cc.begin());
    c.ancestors.push_back(cc);
  }
  return c;
}

uint64_t dec_a(iso::BoxContext& /*ctx*/, ovt::BitReader& /*br*/, iso::FieldBuilder& /*fb*/) {
  return 1;
}
uint64_t dec_b(iso::BoxContext& /*ctx*/, ovt::BitReader& /*br*/, iso::FieldBuilder& /*fb*/) {
  return 2;
}
uint64_t dec_c(iso::BoxContext& /*ctx*/, ovt::BitReader& /*br*/, iso::FieldBuilder& /*fb*/) {
  return 3;
}

// Registered through the public macro into the process-wide registry; a fourcc nothing else uses.
uint64_t dec_macro(iso::BoxContext& /*ctx*/, ovt::BitReader& /*br*/, iso::FieldBuilder& /*fb*/) {
  return 42;
}
OVT_BOX_DECODER("zzt1", "*/moov", &dec_macro)

}  // namespace

TEST_CASE("context patterns", "[container][registry]") {
  const auto m = [](std::string_view pat, std::initializer_list<std::string_view> chain) {
    return iso::context_matches(pat, ctx_of(chain).ancestors);
  };
  CHECK(m("*", {"moov", "trak"}));
  CHECK(m("*/stsd/*", {"moov", "trak", "mdia", "minf", "stbl", "stsd", "hvc1"}));
  CHECK_FALSE(m("*/stsd/*", {"meta", "iprp", "ipco"}));
  CHECK(m("*/ipco", {"meta", "iprp", "ipco"}));
  CHECK_FALSE(m("*/ipco", {"ipco", "colr"}));
  CHECK(m("moov/trak", {"moov", "trak"}));
  CHECK_FALSE(m("moov/trak", {"x   ", "moov", "trak"}));
  CHECK(iso::pattern_specificity("*") == 0);
  CHECK(iso::pattern_specificity("*/ipco") == 1);
  CHECK(iso::pattern_specificity("*/stsd/*") == 1);
  CHECK(iso::pattern_specificity("moov/trak/mdia") == 3);
}

TEST_CASE("the most specific matching pattern wins", "[container][registry]") {
  const iso::BoxDecoder stsd_children{"colr", "*/stsd/*", &dec_a};
  const iso::BoxDecoder ipco{"colr", "*/ipco", &dec_b};
  iso::BoxRegistry reg;
  REQUIRE(reg.add(&stsd_children));
  REQUIRE(reg.add(&ipco));

  SECTION("acceptance: ancestors [meta, iprp, ipco] return the ipco decoder") {
    const auto* e = reg.lookup("colr", ctx_of({"meta", "iprp", "ipco"}));
    REQUIRE(e != nullptr);
    REQUIRE(e->decoder != nullptr);
    CHECK(e->decoder->decode == &dec_b);
    CHECK(std::string_view(e->context_pattern) == "*/ipco");
  }
  SECTION("a sample entry parent returns the stsd decoder") {
    const auto* e = reg.lookup("colr", ctx_of({"moov", "trak", "mdia", "minf", "stbl", "stsd", "hvc1"}));
    REQUIRE(e != nullptr);
    CHECK(e->decoder->decode == &dec_a);
  }
  SECTION("no pattern matches: nullptr") {
    CHECK(reg.lookup("colr", ctx_of({"moov", "udta"})) == nullptr);
    CHECK(reg.lookup("zzzz", ctx_of({"moov"})) == nullptr);
  }
}

TEST_CASE("more literal segments win", "[container][registry]") {
  const iso::BoxDecoder star{"abcd", "*", &dec_a};
  const iso::BoxDecoder tail{"abcd", "*/x   ", &dec_b};
  const iso::BoxDecoder mid{"abcd", "*/x   /*", &dec_c};
  const iso::BoxDecoder exact{"abcd", "y   /x   /z   ", &dec_a};
  iso::BoxRegistry reg;
  for (const auto* d : {&star, &tail, &mid, &exact}) {
    REQUIRE(reg.add(d));
  }
  CHECK(reg.lookup("abcd", ctx_of({"q   "}))->decoder == &star);
  CHECK(reg.lookup("abcd", ctx_of({"q   ", "x   "}))->decoder == &tail);
  // both "*/x/*" (1 literal) and "*/x" (1 literal) can match [x, k]? only the former does
  CHECK(reg.lookup("abcd", ctx_of({"x   ", "k   "}))->decoder == &mid);
  CHECK(reg.lookup("abcd", ctx_of({"y   ", "x   ", "z   "}))->decoder == &exact);
}

TEST_CASE("decoders join the entry table", "[container][registry]") {
  iso::BoxRegistry reg(iso::generated_entries());
  SECTION("a table row without a decoder is a generic shell") {
    const auto* e = reg.lookup("stsd", ctx_of({"moov", "trak", "mdia", "minf", "stbl"}));
    REQUIRE(e != nullptr);
    CHECK(e->kind == iso::BoxKind::ContainerWithPrefix);
    CHECK(e->is_fullbox);
    CHECK(e->decoder == nullptr);
    CHECK_FALSE(std::string_view(e->description).empty());
  }
  SECTION("the colr rows differ by context") {
    const auto* a = reg.lookup("colr", ctx_of({"moov", "trak", "mdia", "minf", "stbl", "stsd", "hvc1"}));
    const auto* b = reg.lookup("colr", ctx_of({"meta", "iprp", "ipco"}));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(std::string_view(a->spec_ref).starts_with("ISO/IEC 14496-12"));
    CHECK(std::string_view(b->spec_ref).starts_with("ISO/IEC 23008-12"));
  }
  SECTION("a decoder keeps the table row's metadata") {
    const iso::BoxDecoder tkhd{"tkhd", "*", &dec_a};
    REQUIRE(reg.add(&tkhd));
    const auto* e = reg.lookup("tkhd", ctx_of({"moov", "trak"}));
    REQUIRE(e != nullptr);
    CHECK(e->decoder == &tkhd);
    CHECK(e->is_fullbox);
    CHECK(e->known_versions_mask == 0x03);
    CHECK(e->kind == iso::BoxKind::Leaf);
  }
}

TEST_CASE("parent wildcard and uuid decoders", "[container][registry]") {
  const iso::BoxDecoder ilst_children{"ilst/*", "*", &dec_a};
  const iso::BoxDecoder exact_name{"name", "*/ilst/*", &dec_b};
  const iso::BoxDecoder uuid{"uuid:8974dbce-7be7-4c51-84f9-7148f9882554", "*", &dec_c};
  iso::BoxRegistry reg;
  REQUIRE(reg.add(&ilst_children));
  REQUIRE(reg.add(&exact_name));
  REQUIRE(reg.add(&uuid));
  CHECK(reg.lookup("data", ctx_of({"moov", "udta", "meta", "ilst"}))->decoder == &ilst_children);
  CHECK(reg.lookup("data", ctx_of({"moov", "udta"})) == nullptr);
  CHECK(reg.lookup("name", ctx_of({"moov", "udta", "meta", "ilst"}))->decoder == &exact_name);
  const std::array<uint8_t, 16> id = {0x89, 0x74, 0xdb, 0xce, 0x7b, 0xe7, 0x4c, 0x51,
                                      0x84, 0xf9, 0x71, 0x48, 0xf9, 0x88, 0x25, 0x54};
  const auto* e = reg.lookup_uuid(id);
  REQUIRE(e != nullptr);
  CHECK(e->decoder == &uuid);
  std::array<uint8_t, 16> other = id;
  other[15] ^= 1U;
  CHECK(reg.lookup_uuid(other) == nullptr);
}

TEST_CASE("OVT_BOX_DECODER registers into the global registry", "[container][registry]") {
  const auto* e = iso::lookup("zzt1", ctx_of({"moov"}));
  REQUIRE(e != nullptr);
  REQUIRE(e->decoder != nullptr);
  CHECK(e->decoder->decode == &dec_macro);
  CHECK(iso::lookup("zzt1", ctx_of({"trak"})) == nullptr);
  // The hand-coded table is reachable through the same call.
  CHECK(iso::lookup("moov", ctx_of({}))->kind == iso::BoxKind::Container);
}
