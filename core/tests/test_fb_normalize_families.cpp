// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.556: the canonical JSON family writers of chapter 14 §14.4.0 (ovt::test::to_canonical), one test case per
// family, over hand-built FlatBuffers.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "ovt_container_generated.h"
#include "ovt_model_generated.h"
#include "test_support/fb_normalize.h"

namespace {

using nlohmann::json;
using ovt::test::Family;
namespace fb = ovt::fb;

flatbuffers::Offset<fb::SyntaxNode> leaf_uint(flatbuffers::FlatBufferBuilder& b, const char* name, uint64_t value,
                                              uint64_t bit_offset, uint32_t bit_length, int array_index = -1) {
  const auto n = b.CreateString(name);
  const fb::BitSpan span(bit_offset, bit_length);
  fb::SyntaxNodeBuilder node(b);
  node.add_name(n);
  node.add_kind(fb::SyntaxValueKind::UInt);
  node.add_uvalue(value);
  node.add_span(&span);
  node.add_array_index(array_index);
  return node.Finish();
}

flatbuffers::Offset<fb::SyntaxNode> container(flatbuffers::FlatBufferBuilder& b, const char* name,
                                              const std::vector<flatbuffers::Offset<fb::SyntaxNode>>& children) {
  const auto n = b.CreateString(name);
  const auto c = b.CreateVector(children);
  fb::SyntaxNodeBuilder node(b);
  node.add_name(n);
  node.add_children(c);
  return node.Finish();
}

// Two units: an SPS (unit 0) and a slice header (unit 1), with elements deliberately out of bit order.
flatbuffers::DetachedBuffer build_stream_index() {
  flatbuffers::FlatBufferBuilder b;
  const auto profile = leaf_uint(b, "profile_idc", 100, 8, 8);
  const auto flag = [&] {
    const auto n = b.CreateString("constraint_set0_flag");
    const fb::BitSpan span(16, 1);
    fb::SyntaxNodeBuilder node(b);
    node.add_name(n);
    node.add_kind(fb::SyntaxValueKind::Bool);
    node.add_bvalue(true);
    node.add_span(&span);
    return node.Finish();
  }();
  const auto level = [&] {
    const auto n = b.CreateString("level_idc");
    const auto e = b.CreateString("Level_4_0");
    const fb::BitSpan span(24, 8);
    fb::SyntaxNodeBuilder node(b);
    node.add_name(n);
    node.add_kind(fb::SyntaxValueKind::Enum);
    node.add_uvalue(40);
    node.add_enum_name(e);
    node.add_span(&span);
    return node.Finish();
  }();
  const auto lf0 = leaf_uint(b, "offset_for_ref_frame", 3, 40, 3, 0);
  const auto lf1 = leaf_uint(b, "offset_for_ref_frame", 5, 43, 3, 1);
  const auto sps = container(b, "sps", {level, flag, profile, lf0, lf1});

  const auto first_mb = leaf_uint(b, "first_mb_in_slice", 0, 0, 1);
  const auto slice = container(b, "slice_header", {first_mb});

  fb::UnitBuilder u0(b);
  u0.add_index(0);
  u0.add_header(sps);
  const auto unit0 = u0.Finish();
  fb::UnitBuilder u1(b);
  u1.add_index(1);
  u1.add_header(slice);
  const auto unit1 = u1.Finish();
  const auto units = b.CreateVector(std::vector<flatbuffers::Offset<fb::Unit>>{unit0, unit1});
  fb::StreamIndexBuilder index(b);
  index.add_units(units);
  b.Finish(index.Finish());
  return b.Release();
}

}  // namespace

TEST_CASE("to_canonical: syntax-elements sorted by (unit, bit_offset) with typed values",
          "[test_support][golden][fast]") {
  const auto buffer = build_stream_index();
  const auto* index = flatbuffers::GetRoot<fb::StreamIndex>(buffer.data());
  const json elements = ovt::test::to_canonical(*index, Family::kSyntaxElements);
  REQUIRE(elements.is_array());
  REQUIRE(elements.size() == 6);
  CHECK(elements[0] == json::parse(R"({"unit":0,"name":"profile_idc","bit_offset":8,"bit_length":8,"value":100})"));
  CHECK(elements[1] ==
        json::parse(R"({"unit":0,"name":"constraint_set0_flag","bit_offset":16,"bit_length":1,"value":1})"));
  CHECK(elements[2]["name"] == "level_idc");
  CHECK(elements[2]["value"] == "Level_4_0");
  CHECK(elements[3]["name"] == "offset_for_ref_frame");
  CHECK(elements[4]["bit_offset"] == 43);
  CHECK(elements[5] == json::parse(R"({"unit":1,"name":"first_mb_in_slice","bit_offset":0,"bit_length":1,"value":0})"));
}

TEST_CASE("to_canonical: header-struct nests by name and collects repeats into arrays",
          "[test_support][golden][fast]") {
  const auto buffer = build_stream_index();
  const auto* index = flatbuffers::GetRoot<fb::StreamIndex>(buffer.data());
  const json headers = ovt::test::to_canonical(*index, Family::kHeaderStruct);
  CHECK(headers == json::parse(R"({
    "sps": {"profile_idc":100,"constraint_set0_flag":1,"level_idc":"Level_4_0","offset_for_ref_frame":[3,5]},
    "slice_header": {"first_mb_in_slice":0}})"));

  // One header on its own, with the unit index of the caller.
  const auto* sps = index->units()->Get(0)->header();
  CHECK(ovt::test::to_canonical(*sps, Family::kSyntaxElements, 7)[0]["unit"] == 7);
  CHECK(ovt::test::to_canonical(*sps, Family::kHeaderStruct).contains("sps"));
  CHECK_THROWS_AS(ovt::test::to_canonical(*sps, Family::kBoxTree), std::invalid_argument);
  CHECK_THROWS_AS(ovt::test::to_canonical(*index, Family::kScores), std::invalid_argument);
}

TEST_CASE("to_canonical: box-tree paths, offsets, sizes and fields", "[test_support][golden][fast]") {
  flatbuffers::FlatBufferBuilder b;
  const auto field_name = b.CreateString("timescale");
  fb::FieldBuilder timescale(b);
  timescale.add_name(field_name);
  timescale.add_kind(fb::SyntaxValueKind::UInt);
  timescale.add_uvalue(90000);
  const auto timescale_off = timescale.Finish();
  const auto brand_name = b.CreateString("major_brand");
  const auto brand_value = b.CreateString("isom");
  fb::FieldBuilder brand(b);
  brand.add_name(brand_name);
  brand.add_kind(fb::SyntaxValueKind::String);
  brand.add_svalue(brand_value);
  const auto brand_off = brand.Finish();
  // A named struct field with children, and an array field with unnamed children.
  const auto entry_name = b.CreateString("entry");
  const auto child_a_name = b.CreateString("a");
  fb::FieldBuilder child_a(b);
  child_a.add_name(child_a_name);
  child_a.add_kind(fb::SyntaxValueKind::Int);
  child_a.add_ivalue(-2);
  const auto child_a_off = child_a.Finish();
  const auto entry_children = b.CreateVector(std::vector<flatbuffers::Offset<fb::Field>>{child_a_off});
  fb::FieldBuilder entry(b);
  entry.add_name(entry_name);
  entry.add_children(entry_children);
  const auto entry_off = entry.Finish();

  const auto make_box = [&](const char* type, uint64_t offset, uint64_t size,
                            const std::vector<flatbuffers::Offset<fb::Field>>& fields,
                            const std::vector<flatbuffers::Offset<fb::BoxNode>>& children) {
    const auto f = b.CreateString(type);
    const auto fv = b.CreateVector(fields);
    const auto cv = b.CreateVector(children);
    const fb::ByteSpan span(offset, size);
    fb::BoxNodeBuilder box(b);
    box.add_fourcc(f);
    box.add_file_span(&span);
    box.add_fields(fv);
    box.add_children(cv);
    return box.Finish();
  };
  const auto trak1 = make_box("trak", 100, 40, {timescale_off, entry_off}, {});
  const auto trak2 = make_box("trak", 140, 40, {}, {});
  const auto udta = make_box("udta", 180, 8, {}, {});
  const auto moov = make_box("moov", 28, 160, {}, {trak1, trak2, udta});
  const auto ftyp = make_box("ftyp", 0, 28, {brand_off}, {});
  const auto roots = b.CreateVector(std::vector<flatbuffers::Offset<fb::BoxNode>>{ftyp, moov});
  fb::ContainerTreeBuilder tree(b);
  tree.add_root(roots);
  b.Finish(tree.Finish());

  const auto* t = flatbuffers::GetRoot<fb::ContainerTree>(b.GetBufferPointer());
  const json boxes = ovt::test::to_canonical(*t);
  CHECK(boxes == json::parse(R"([
    {"path":"ftyp","offset":0,"size":28,"fields":{"major_brand":"isom"}},
    {"path":"moov","offset":28,"size":160,"fields":{}},
    {"path":"moov/trak[1]","offset":100,"size":40,"fields":{"timescale":90000,"entry":{"a":-2}}},
    {"path":"moov/trak[2]","offset":140,"size":40,"fields":{}},
    {"path":"moov/udta","offset":180,"size":8,"fields":{}}])"));
  CHECK_THROWS_AS(ovt::test::to_canonical(*t, Family::kScores), std::invalid_argument);
}
