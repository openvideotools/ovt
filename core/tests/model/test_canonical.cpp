// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.085: canonicalize() (rule V-032) and structural_diff() (`ovt dump --diff`, chapter 12 §12.2.6) on the
// §6.10 fixture tiny16_hevc.fa of WI-0.093. Mutated copies are made with the object API (UnPack, edit, Pack).
// SPEC-ASSUMPTION(#271): AT2 edits both copies (qp[5] = 30 and 31); the fixture itself has 32 there.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "ovt/cxx/model/canonical.hpp"
#include "ovt/cxx/model/schema_id.hpp"
#include "ovt_model_generated.h"
#include "sha256.hpp"

namespace {

using ovt::golden::Bytes;
using ovt::model::SchemaId;
namespace fb = ovt::fb;

constexpr const char* kRoot = "ovt.fb.FrameAnalysis";

Bytes read_file(const std::string& name) {
  std::ifstream in(std::filesystem::path(OVT_GOLDEN_DIR) / name, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The recorded hash: first field of `<name>.sha256` (`<hex>  <name>`).
std::string recorded_sha(const std::string& name) {
  const Bytes line = read_file(name + ".sha256");
  return {line.begin(), line.begin() + 64};
}

Bytes fixture() {
  return read_file("tiny16_hevc.fa");
}

/// UnPack @p bytes, apply @p edit, Pack again (ForceDefaults as given) with the `OVTA` identifier.
Bytes edited(const Bytes& bytes, const std::function<void(fb::FrameAnalysisT&)>& edit, bool force_defaults = false) {
  fb::FrameAnalysisT native;
  fb::GetFrameAnalysis(bytes.data())->UnPackTo(&native);
  edit(native);
  flatbuffers::FlatBufferBuilder builder;
  builder.ForceDefaults(force_defaults);
  fb::FinishFrameAnalysisBuffer(builder, fb::FrameAnalysis::Pack(builder, &native));
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

/// A nested table buffer packed with ForceDefaults(true) (non-canonical: defaults written out).
template <class Table>
Bytes forced_nested(const Bytes& nested_bytes) {
  typename Table::NativeTableType native;
  flatbuffers::GetRoot<Table>(nested_bytes.data())->UnPackTo(&native);
  flatbuffers::FlatBufferBuilder builder;
  builder.ForceDefaults(true);
  builder.Finish(Table::Pack(builder, &native));
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

}  // namespace

TEST_CASE("test_canonical: tiny16_hevc.fa canonicalises to its recorded SHA-256 (AT1)", "[model][golden][fast]") {
  const Bytes file = fixture();
  const auto canonical = ovt::model::canonicalize(file, SchemaId::Model, kRoot);
  CHECK(ovt::golden::sha256_hex(canonical) == recorded_sha("tiny16_hevc.fa"));
  CHECK(canonical == file);
}

TEST_CASE("test_canonical: canonicalize is idempotent and deterministic", "[model][fast]") {
  const Bytes file = fixture();
  const auto once = ovt::model::canonicalize(file, SchemaId::Model, kRoot);
  const auto twice = ovt::model::canonicalize(once, SchemaId::Model, kRoot);
  CHECK(once == twice);
  CHECK(ovt::model::canonicalize(file, SchemaId::Model, kRoot) == once);
}

TEST_CASE("test_canonical: a ForceDefaults producer canonicalises to the fixture bytes", "[model][fast]") {
  const Bytes file = fixture();
  const Bytes forced = edited(file, [](fb::FrameAnalysisT&) {}, /*force_defaults=*/true);
  REQUIRE(forced != file);  // non-canonical: V-032 would fire on it
  CHECK(ovt::model::canonicalize(forced, SchemaId::Model, kRoot) == file);
  CHECK(ovt::model::structural_diff(forced, file, SchemaId::Model, kRoot).empty());
}

TEST_CASE("test_canonical: nested buffers are canonicalised recursively", "[model][fast]") {
  const Bytes file = fixture();
  const Bytes forced = edited(file, [](fb::FrameAnalysisT& fa) {
    REQUIRE(fa.blocks.at(2)->ext_type == "HevcCu");
    fa.blocks.at(2)->ext = forced_nested<fb::HevcCu>(fa.blocks.at(2)->ext);
    REQUIRE(fa.frame->ref_state_type == "HevcRps");
    fa.frame->ref_state = forced_nested<fb::HevcRps>(fa.frame->ref_state);
    REQUIRE(fa.filter_params_type == "HevcFilterParams");
    fa.filter_params = forced_nested<fb::HevcFilterParams>(fa.filter_params);
  });
  REQUIRE(forced != file);
  CHECK(ovt::model::canonicalize(forced, SchemaId::Model, kRoot) == file);
}

TEST_CASE("test_canonical: an empty non-required string or vector is canonically absent", "[model][fast]") {
  // Build a buffer the object API never writes: an empty plugin_version string and an empty planes vector.
  flatbuffers::FlatBufferBuilder builder;
  const auto plugin_id = builder.CreateString("p");
  const auto empty_string = builder.CreateString("");
  const auto empty_planes = builder.CreateVector(std::vector<flatbuffers::Offset<fb::PlaneRef>>{});
  fb::FrameAnalysisBuilder fa(builder);
  fa.add_schema_version(1);
  fa.add_plugin_id(plugin_id);
  fa.add_plugin_version(empty_string);
  fa.add_planes(empty_planes);
  fb::FinishFrameAnalysisBuffer(builder, fa.Finish());
  const Bytes with_empties(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());

  const Bytes expected = edited(with_empties, [](fb::FrameAnalysisT&) {});
  REQUIRE(expected != with_empties);
  CHECK(ovt::model::canonicalize(with_empties, SchemaId::Model, kRoot) == expected);
  CHECK(ovt::model::structural_diff(with_empties, expected, SchemaId::Model, kRoot).empty());
}

TEST_CASE("test_canonical: structural_diff reports grid.qp[5] (AT2)", "[model][fast]") {
  const Bytes file = fixture();
  const Bytes a = edited(file, [](fb::FrameAnalysisT& fa) { fa.grid->qp.at(5) = 30; });
  const Bytes b = edited(file, [](fb::FrameAnalysisT& fa) { fa.grid->qp.at(5) = 31; });
  CHECK(ovt::model::structural_diff(a, b, SchemaId::Model, kRoot) == std::vector<std::string>{"grid.qp[5]: 30 != 31"});
  CHECK(ovt::model::structural_diff(file, file, SchemaId::Model, kRoot).empty());
  CHECK(ovt::model::structural_diff(a, a, SchemaId::Model, kRoot).empty());
}

TEST_CASE("test_canonical: structural_diff paths, formats and order", "[model][fast]") {
  const Bytes file = fixture();
  const Bytes b = edited(file, [](fb::FrameAnalysisT& fa) {
    fa.plugin_id = "other";                                           // string
    fa.frame->type = fb::FrameType::P;                                // enum, in a sub-table
    fa.frame->shown = false;                                          // bool
    fa.frame->coded_size = std::make_unique<fb::Rect>(0, 0, 32, 16);  // struct field
    fa.grid->qp.pop_back();                                           // length change
    fa.blocks.at(3)->qp = 7;                                          // vector of tables
    fb::HevcCuT cu;                                                   // nested buffer (blocks[2].ext)
    flatbuffers::GetRoot<fb::HevcCu>(fa.blocks.at(2)->ext.data())->UnPackTo(&cu);
    cu.QpY = 33;
    flatbuffers::FlatBufferBuilder nested;
    nested.Finish(fb::HevcCu::Pack(nested, &cu));
    fa.blocks.at(2)->ext.assign(nested.GetBufferPointer(), nested.GetBufferPointer() + nested.GetSize());
  });
  const auto diff = ovt::model::structural_diff(file, b, SchemaId::Model, kRoot);
  // Lines follow field-id (schema declaration) order, depth first.
  CHECK(diff == std::vector<std::string>{
                    "plugin_id: \"ovt-plugin-de265\" != \"other\"",
                    "frame.type: I != P",
                    "frame.shown: true != false",
                    "frame.coded_size.w: 16 != 32",
                    "grid.qp.length: 16 != 15",
                    "blocks[2].ext.QpY: 32 != 33",
                    "blocks[3].qp: 32 != 7",
                });
}

TEST_CASE("test_canonical: invalid input is rejected", "[model][fast]") {
  const Bytes file = fixture();
  const Bytes truncated(file.begin(), file.begin() + 32);
  CHECK_THROWS_AS(ovt::model::canonicalize(truncated, SchemaId::Model, kRoot), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::canonicalize({}, SchemaId::Model, kRoot), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::canonicalize(file, SchemaId::Model, "ovt.fb.NoSuchTable"), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::canonicalize(file, SchemaId::Model, "ovt.fb.Rect"), std::invalid_argument);  // struct
  CHECK_THROWS_AS(ovt::model::structural_diff(file, truncated, SchemaId::Model, kRoot), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::structural_diff(truncated, file, SchemaId::Model, kRoot), std::invalid_argument);

  // A nested buffer that does not verify as the table its `_type` names.
  const Bytes bad_nested = edited(file, [](fb::FrameAnalysisT& fa) { fa.blocks.at(2)->ext = {1, 2, 3}; });
  CHECK_THROWS_AS(ovt::model::canonicalize(bad_nested, SchemaId::Model, kRoot), std::invalid_argument);
  // The diff falls back to comparing such bytes element by element.
  const auto diff = ovt::model::structural_diff(file, bad_nested, SchemaId::Model, kRoot);
  REQUIRE_FALSE(diff.empty());
  CHECK(diff.front().rfind("blocks[2].ext.length: ", 0) == 0);
}

namespace {

/// A FrameAnalysis whose filter_params nests another FrameAnalysis, @p levels deep (`_type` may name any
/// table, so untrusted input can do this).
Bytes nested_chain(int levels) {
  fb::FrameAnalysisT native;
  native.schema_version = 1;
  native.plugin_id = "p";
  flatbuffers::FlatBufferBuilder inner;
  inner.Finish(fb::FrameAnalysis::Pack(inner, &native));
  Bytes bytes(inner.GetBufferPointer(), inner.GetBufferPointer() + inner.GetSize());
  for (int level = 0; level < levels; ++level) {
    native.filter_params = bytes;
    native.filter_params_type = "FrameAnalysis";
    flatbuffers::FlatBufferBuilder outer;
    outer.Finish(fb::FrameAnalysis::Pack(outer, &native));
    bytes.assign(outer.GetBufferPointer(), outer.GetBufferPointer() + outer.GetSize());
  }
  return bytes;
}

}  // namespace

TEST_CASE("test_canonical: nested-buffer recursion is bounded", "[model][fast]") {
  const Bytes ok = nested_chain(4);
  const Bytes changed = edited(ok, [](fb::FrameAnalysisT& fa) { fa.plugin_id = "q"; });
  CHECK(ovt::model::canonicalize(ok, SchemaId::Model, kRoot) == ok);
  CHECK(ovt::model::structural_diff(ok, changed, SchemaId::Model, kRoot) ==
        std::vector<std::string>{"plugin_id: \"p\" != \"q\""});

  const Bytes deep = nested_chain(5);
  CHECK_THROWS_AS(ovt::model::canonicalize(deep, SchemaId::Model, kRoot), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::structural_diff(deep, deep, SchemaId::Model, kRoot), std::invalid_argument);

  const Bytes very_deep = nested_chain(200);  // would recurse 200 levels without the cap
  CHECK_THROWS_AS(ovt::model::canonicalize(very_deep, SchemaId::Model, kRoot), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::structural_diff(very_deep, very_deep, SchemaId::Model, kRoot), std::invalid_argument);
}
