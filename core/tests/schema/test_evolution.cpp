// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.094 (chapter 06 §6.11): the additive-evolution rule of §6.1 P7, proved on the WI-0.093 fixtures.
//
// `ovt_model_v1_plus.fbs` (evolution/, generated from the real schema by ../make_v1_plus.cmake) is schema major 1
// with one field appended to FrameAnalysis (`evo_note`), Block (`evo_score`, default 7) and CellGrid
// (`evo_cells`) and one value appended to PredKind (`EvoFuture`), in namespace ovt.fb.evo. It plays a later
// minor version of the schema:
//   - a newer reader (ovt.fb.evo) reads today's buffers: every old field reads identically, new fields default;
//   - an older reader (ovt.fb) reads a newer writer's buffer: it verifies, the unknown field is ignored, and a
//     rebuild by the older reader (object API, or canonicalize() of WI-0.085) drops the newer field.
// The last point is expected: canonicalisation by an older reader loses newer fields, which is why the disk
// cache stores the writer's bytes and never canonicalised bytes (chapter 06 §6.7).
//
// Acceptance test 3: a `grid.block_id` length field claiming 2^31 elements is rejected by the Verifier before
// any validator rule runs.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/model/validator.hpp"
#include "ovt_model_generated.h"
#include "ovt_model_v1_plus_generated.h"

// SPEC-ASSUMPTION(#435): canonicalize() is WI-0.085; its assertions switch on when that header exists.
#if __has_include("ovt/cxx/model/canonical.hpp")
#include "ovt/cxx/model/canonical.hpp"
#define OVT_EVO_HAVE_CANONICALIZE 1
#else
#define OVT_EVO_HAVE_CANONICALIZE 0
#endif

namespace {

using Bytes = std::vector<std::uint8_t>;
namespace fb = ovt::fb;
namespace evo = ovt::fb::evo;

constexpr std::array<std::string_view, 3> kFixtures = {"tiny16_hevc.fa", "tiny32_h264.fa", "tiny64_av1.fa"};

Bytes read_fixture(std::string_view name) {
  std::ifstream in(std::filesystem::path(OVT_GOLDEN_DIR) / std::string(name), std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool verify_real(const Bytes& bytes) {
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  return fb::VerifyFrameAnalysisBuffer(verifier);
}

bool verify_evo(const Bytes& bytes) {
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  return evo::VerifyFrameAnalysisBuffer(verifier);
}

template <class Root, class Native>
Bytes pack(const Native& native) {
  flatbuffers::FlatBufferBuilder builder;  // ForceDefaults off: the canonical object-API layout
  builder.Finish(Root::Pack(builder, &native), "OVTA");
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

template <class T>
std::vector<T> to_vector(const flatbuffers::Vector<T>* v) {
  return v == nullptr ? std::vector<T>{} : std::vector<T>(v->begin(), v->end());
}

// The old fields of one buffer as seen by the real reader and by the v1_plus reader are the same values.
// (The byte-identical repack in the first test covers every field; these spot checks name the tables the
// evolution touched, so a failure points at the right place.)
void check_same_old_fields(const fb::FrameAnalysis& real, const evo::FrameAnalysis& plus) {
  CHECK(plus.schema_version() == real.schema_version());
  CHECK(plus.plugin_id()->str() == real.plugin_id()->str());
  CHECK(plus.frame()->poc() == real.frame()->poc());

  const auto* rg = real.grid();
  const auto* pg = plus.grid();
  REQUIRE(rg != nullptr);
  REQUIRE(pg != nullptr);
  CHECK(pg->cells_w() == rg->cells_w());
  CHECK(pg->cells_h() == rg->cells_h());
  CHECK(to_vector(pg->block_id()) == to_vector(rg->block_id()));
  CHECK(to_vector(pg->qp()) == to_vector(rg->qp()));
  CHECK(to_vector(pg->pred()) == to_vector(rg->pred()));
  CHECK(to_vector(pg->mv0_x()) == to_vector(rg->mv0_x()));
  CHECK(to_vector(pg->mv0_y()) == to_vector(rg->mv0_y()));
  CHECK(to_vector(pg->filter_idx()) == to_vector(rg->filter_idx()));  // the last old CellGrid field

  REQUIRE(plus.blocks()->size() == real.blocks()->size());
  for (flatbuffers::uoffset_t i = 0; i < real.blocks()->size(); ++i) {
    const auto* rb = real.blocks()->Get(i);
    const auto* pb = plus.blocks()->Get(i);
    INFO("blocks[" << i << "]");
    CHECK(pb->id() == rb->id());
    CHECK(pb->parent() == rb->parent());
    CHECK(to_vector(pb->children()) == to_vector(rb->children()));
    CHECK(static_cast<int>(pb->kind()) == static_cast<int>(rb->kind()));
    CHECK(static_cast<int>(pb->pred()) == static_cast<int>(rb->pred()));
    CHECK(std::string_view(evo::EnumNamePredKind(pb->pred())) == fb::EnumNamePredKind(rb->pred()));
    CHECK(pb->qp() == rb->qp());
    CHECK(pb->bits_x8() == rb->bits_x8());
    CHECK(flatbuffers::GetString(pb->ext_type()) == flatbuffers::GetString(rb->ext_type()));
    CHECK(to_vector(pb->ext()) == to_vector(rb->ext()));  // the last old Block field
  }
  CHECK(to_vector(plus.qp_hist()) == to_vector(real.qp_hist()));
  CHECK(flatbuffers::VectorLength(plus.diagnostics()) == flatbuffers::VectorLength(real.diagnostics()));
}

// A v1_plus buffer built from @p fixture with every evolution field set to a non-default value.
Bytes write_with_v1_plus(const Bytes& fixture) {
  evo::FrameAnalysisT native;
  evo::GetFrameAnalysis(fixture.data())->UnPackTo(&native);
  native.evo_note = "written by a newer minor version";
  REQUIRE(native.grid != nullptr);
  native.grid->evo_cells.assign(std::size_t{native.grid->cells_w} * native.grid->cells_h, std::uint16_t{0xBEEF});
  for (auto& block : native.blocks) {
    block->evo_score = 99;
  }
  return pack<evo::FrameAnalysis>(native);
}

std::vector<ovt::model::Violation> without_v032(std::vector<ovt::model::Violation> violations) {
  // SPEC-ASSUMPTION(#435): V-032 (canonical bytes) cannot hold for a newer writer's buffer seen by an older validator:
  // the older canonical form drops the newer field (expected, see the file comment). Every other rule must pass.
  std::erase_if(violations, [](const ovt::model::Violation& v) { return v.rule == "V-032"; });
  return violations;
}

}  // namespace

TEST_CASE("test_evolution: v1_plus reads the fixtures, old fields identical, new fields default (AT1)",
          "[schema][evolution][fast]") {
  for (const auto name : kFixtures) {
    INFO(name);
    const Bytes file = read_fixture(name);
    REQUIRE(verify_real(file));
    REQUIRE(verify_evo(file));  // a newer reader accepts today's buffer

    const auto* real = fb::GetFrameAnalysis(file.data());
    const auto* plus = evo::GetFrameAnalysis(file.data());
    check_same_old_fields(*real, *plus);

    // New fields read their defaults.
    CHECK(plus->evo_note() == nullptr);
    CHECK(plus->grid()->evo_cells() == nullptr);
    for (const auto* block : *plus->blocks()) {
      CHECK(block->evo_score() == 7U);
    }
    evo::FrameAnalysisT native;
    plus->UnPackTo(&native);
    CHECK(native.evo_note.empty());
    CHECK(native.grid->evo_cells.empty());
    CHECK(std::all_of(native.blocks.begin(), native.blocks.end(), [](const auto& b) { return b->evo_score == 7U; }));

    // Every old field, read through v1_plus and written back with the new fields at their defaults, gives
    // the fixture byte for byte: nothing was lost or reinterpreted (the fixtures are in object-API form,
    // WI-0.093 test_model_roundtrip).
    CHECK(pack<evo::FrameAnalysis>(native) == file);
  }
}

TEST_CASE("test_evolution: the real schema reads a v1_plus buffer and ignores the unknown fields (AT1)",
          "[schema][evolution][fast]") {
  for (const auto name : kFixtures) {
    INFO(name);
    const Bytes file = read_fixture(name);
    const Bytes newer = write_with_v1_plus(file);
    REQUIRE(newer != file);
    REQUIRE(verify_evo(newer));
    {
      const auto* plus = evo::GetFrameAnalysis(newer.data());
      REQUIRE(plus->evo_note() != nullptr);
      CHECK(plus->evo_note()->str() == "written by a newer minor version");
      CHECK(flatbuffers::VectorLength(plus->grid()->evo_cells()) ==
            std::size_t{plus->grid()->cells_w()} * plus->grid()->cells_h());
      CHECK(plus->blocks()->Get(0)->evo_score() == 99U);
    }

    // The older reader: verifies, reads every old field identically, never sees the new ones.
    REQUIRE(verify_real(newer));
    const auto* old_view = fb::GetFrameAnalysis(newer.data());
    check_same_old_fields(*old_view, *evo::GetFrameAnalysis(file.data()));
    CHECK(without_v032(ovt::model::validate_frame_analysis(newer)).empty());

    // A rebuild by the older reader (object API, ForceDefaults off) drops the unknown fields and gives back
    // exactly today's bytes.
    fb::FrameAnalysisT native;
    old_view->UnPackTo(&native);
    CHECK(pack<fb::FrameAnalysis>(native) == file);

#if OVT_EVO_HAVE_CANONICALIZE
    // canonicalize() (WI-0.085) by the older reader drops the newer fields too: expected, and the reason the
    // cache keeps the writer's bytes.
    const auto canonical = ovt::model::canonicalize(newer, ovt::model::SchemaId::Model, "ovt.fb.FrameAnalysis");
    CHECK(canonical == file);
    CHECK(ovt::model::structural_diff(newer, file, ovt::model::SchemaId::Model, "ovt.fb.FrameAnalysis").empty());
#endif
  }
}

TEST_CASE("test_evolution: an appended enum value reaches an older reader as its raw number",
          "[schema][evolution][fast]") {
  const Bytes file = read_fixture("tiny16_hevc.fa");
  evo::FrameAnalysisT native;
  evo::GetFrameAnalysis(file.data())->UnPackTo(&native);
  REQUIRE(native.blocks.size() > 1U);
  native.blocks[1]->pred = evo::PredKind::EvoFuture;
  const Bytes newer = pack<evo::FrameAnalysis>(native);

  REQUIRE(verify_real(newer));  // FlatBuffers does not range-check enums: the buffer stays readable
  const auto* old_view = fb::GetFrameAnalysis(newer.data());
  const auto pred = old_view->blocks()->Get(1)->pred();
  CHECK(static_cast<std::uint8_t>(pred) == static_cast<std::uint8_t>(evo::PredKind::EvoFuture));
  CHECK(static_cast<std::uint8_t>(pred) > static_cast<std::uint8_t>(fb::PredKind::MAX));
  CHECK(std::string_view(fb::EnumNamePredKind(pred)).empty());  // no name in the older schema
  // The validator must not crash on it; whether a rule reports it is the rule's business.
  CHECK_NOTHROW(ovt::model::validate_frame_analysis(newer));
}

TEST_CASE("test_evolution: a block_id length of 2^31 is rejected by the Verifier before any rule (AT3)",
          "[schema][evolution][validator][fast]") {
  Bytes bytes = read_fixture("tiny16_hevc.fa");
  const auto* grid = fb::GetFrameAnalysis(bytes.data())->grid();
  REQUIRE(grid != nullptr);
  REQUIRE(grid->block_id() != nullptr);
  // A flatbuffers::Vector starts at its uoffset_t length field.
  const auto at = static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(grid->block_id()) - bytes.data());
  REQUIRE(at + sizeof(flatbuffers::uoffset_t) <= bytes.size());
  flatbuffers::WriteScalar<flatbuffers::uoffset_t>(bytes.data() + at, flatbuffers::uoffset_t{1} << 31U);
  REQUIRE(fb::GetFrameAnalysis(bytes.data())->grid()->block_id()->size() == (std::uint32_t{1} << 31U));

  CHECK_FALSE(verify_real(bytes));
  // The validator's own Verifier options (max_depth 64, max_tables 10'000'000) reject it as well.
  flatbuffers::Verifier::Options opts;
  opts.max_depth = 64;
  opts.max_tables = 10'000'000;
  flatbuffers::Verifier wide(bytes.data(), bytes.size(), opts);
  CHECK_FALSE(fb::VerifyFrameAnalysisBuffer(wide));

  // V-001 stops validation: exactly one violation, no other rule ran (so nothing sized by the claimed length
  // was allocated or walked).
  const auto violations = ovt::model::validate_frame_analysis(bytes);
  REQUIRE(violations.size() == 1U);
  CHECK(violations[0].rule == "V-001");
}
