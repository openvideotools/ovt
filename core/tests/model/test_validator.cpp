// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.086: the §6.9 validator on the WI-0.093 golden fixtures. Acceptance test 1 (the fixtures are valid),
// 2 (each mutated fixture makes exactly its rule fire, table in mutate.h), 3 (truncated buffers report V-001
// and never crash; run under linux-x64-asan-ubsan in CI), plus the rule table, the per-rule cap, the
// partial-record downgrade and the size-prefixed form.

#include "ovt/cxx/model/validator.hpp"

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "mutate.h"
#include "ovt_model_generated.h"

namespace {

using ovt::model::Violation;
using ovt::test::Bytes;
using ovt::test::Fixture;
namespace fb = ovt::fb;

Bytes read_fixture(Fixture fixture) {
  const auto name = ovt::test::kFixtureFiles.at(static_cast<std::size_t>(fixture));
  std::ifstream in(std::filesystem::path(OVT_GOLDEN_DIR) / name, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::array<std::uint8_t, 32> fixture_hash() {
  std::array<std::uint8_t, 32> hash{};
  const auto hex = ovt::test::kFixtureSourceHash;
  const auto nibble = [](char ch) { return static_cast<std::uint8_t>(ch <= '9' ? ch - '0' : ch - 'a' + 10); };
  for (std::size_t i = 0; i < hash.size(); ++i) {
    hash.at(i) = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4U) | nibble(hex[(2 * i) + 1]));
  }
  return hash;
}

std::vector<Violation> validate(const Bytes& buffer) {
  return ovt::model::validate_frame_analysis(buffer, fixture_hash());
}

std::set<std::string> rule_ids(const std::vector<Violation>& violations) {
  std::set<std::string> ids;
  for (const auto& v : violations) {
    ids.insert(v.rule);
  }
  return ids;
}

std::string describe(const std::vector<Violation>& violations) {
  std::string out;
  for (const auto& v : violations) {
    out += v.rule + " " + v.path + ": " + v.message + "\n";
  }
  return out;
}

constexpr std::array<Fixture, 3> kAllFixtures{Fixture::Hevc16, Fixture::H264_32, Fixture::Av1_64};

}  // namespace

TEST_CASE("validator AT1: the golden fixtures have no violations", "[model][validator][fast]") {
  for (const auto fixture : kAllFixtures) {
    INFO(ovt::test::kFixtureFiles.at(static_cast<std::size_t>(fixture)));
    const auto violations = validate(read_fixture(fixture));
    INFO(describe(violations));
    CHECK(violations.empty());
    CHECK(ovt::model::validate_frame_analysis(read_fixture(fixture)).empty());  // no hash known
  }
}

TEST_CASE("validator AT2: each mutated fixture makes exactly its rule fire", "[model][validator][fast]") {
  for (const auto& row : ovt::test::rule_mutations()) {
    INFO(std::string(row.rule) + ": " + std::string(row.what));
    const auto violations = validate(ovt::test::mutate(read_fixture(row.fixture), row.mutate));
    INFO(describe(violations));
    CHECK(rule_ids(violations) == std::set<std::string>{std::string(row.rule)});
  }
}

TEST_CASE("validator AT3: truncated and corrupted buffers report V-001 and never crash", "[model][validator][fast]") {
  for (const auto fixture : kAllFixtures) {
    const Bytes full = read_fixture(fixture);
    for (std::size_t len = 0; len < full.size(); len += (len < 64 ? 1 : 7)) {
      const std::span<const std::uint8_t> truncated(full.data(), len);
      const auto violations = ovt::model::validate_frame_analysis(truncated, fixture_hash());
      INFO("length " << len << "\n" << describe(violations));
      REQUIRE(violations.size() == 1);
      CHECK(violations[0].rule == "V-001");
      CHECK(violations[0].severity == fb::Severity::Error);
    }
    // Single-byte corruption: any result is acceptable as long as nothing crashes (ASan/UBSan in CI).
    for (std::size_t pos = 0; pos < full.size(); ++pos) {
      Bytes corrupt = full;
      corrupt[pos] ^= 0xA5U;
      (void)ovt::model::validate_frame_analysis(corrupt, fixture_hash());
    }
  }
}

TEST_CASE("validator: the rule table lists V-001..V-032 in order", "[model][validator][fast]") {
  const auto rules = ovt::model::Validator::rules();
  REQUIRE(rules.size() == 32);
  for (std::size_t i = 0; i < rules.size(); ++i) {
    const std::string expected = std::string("V-0") + (i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
    CHECK(rules[i].id == expected);
    CHECK_FALSE(rules[i].intent.empty());
  }
}

TEST_CASE("validator: violations beyond the per-rule cap are summarised", "[model][validator][fast]") {
  // Every one of the 256 AV1 cells points at the root: 256 V-010 violations, 64 listed plus one summary.
  const auto violations = validate(ovt::test::mutate(read_fixture(Fixture::Av1_64), [](fb::FrameAnalysisT& fa) {
    std::fill(fa.grid->block_id.begin(), fa.grid->block_id.end(), 0U);
  }));
  std::size_t v010 = 0;
  for (const auto& v : violations) {
    v010 += v.rule == "V-010" ? 1U : 0U;
  }
  CHECK(v010 == ovt::model::kMaxViolationsPerRule + 1);
  const auto summary =
      std::find_if(violations.rbegin(), violations.rend(), [](const Violation& v) { return v.rule == "V-010"; });
  REQUIRE(summary != violations.rend());
  CHECK(summary->message == "... and 192 more violations of this rule");
}

TEST_CASE("validator: a Fatal diagnostic downgrades the grid and tree rules to Warning", "[model][validator][fast]") {
  const auto violations = validate(ovt::test::mutate(read_fixture(Fixture::Hevc16), [](fb::FrameAnalysisT& fa) {
    auto fatal = std::make_unique<fb::DiagnosticT>();
    fatal->severity = fb::Severity::Fatal;
    fatal->code = "DEC-0001";
    fatal->message = "decoding stopped";
    fa.diagnostics.push_back(std::move(fatal));
    fa.grid->block_id[0] = 0;    // V-010: downgraded
    fa.grid->bits_x8.clear();    // (keeps V-020 quiet)
    fa.frame->timebase_den = 0;  // V-002: stays an Error
  }));
  INFO(describe(violations));
  REQUIRE(rule_ids(violations) == std::set<std::string>{"V-002", "V-010"});
  for (const auto& v : violations) {
    CHECK(v.severity == (v.rule == "V-010" ? fb::Severity::Warning : fb::Severity::Error));
  }
}

TEST_CASE("validator: size-prefixed buffers and the codec override", "[model][validator][fast]") {
  const Bytes plain = read_fixture(Fixture::Hevc16);
  fb::FrameAnalysisT native;
  fb::GetFrameAnalysis(plain.data())->UnPackTo(&native);
  flatbuffers::FlatBufferBuilder builder;
  fb::FinishSizePrefixedFrameAnalysisBuffer(builder, fb::FrameAnalysis::Pack(builder, &native));
  const Bytes prefixed(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
  CHECK(validate(prefixed).empty());

  // An HEVC tree checked as AV1 breaks the codec-dependent rules.
  const auto as_av1 = ovt::model::validate_frame_analysis(plain, fixture_hash(), fb::Codec::AV1);
  CHECK(rule_ids(as_av1).contains("V-008"));
}
