// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fixture mutators for the validator tests (chapter 06 WI-0.086 step 6): each one takes a WI-0.093 golden
// fixture through the object API, breaks exactly one §6.9 rule with a minimal edit, and repacks it. The
// edits are chosen so that no other rule notices them (e.g. a mutation that moves a cell to another block
// also clears grid.bits_x8, which V-020 would otherwise check); test_validator asserts that only the intended
// rule fires.

#ifndef OVT_TESTS_MODEL_MUTATE_H
#define OVT_TESTS_MODEL_MUTATE_H

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "ovt_model_generated.h"

namespace ovt::test {

namespace fb = ovt::fb;

using Bytes = std::vector<std::uint8_t>;
using Mutator = void (*)(fb::FrameAnalysisT&);

/// Golden fixture stems of core/tests/golden/ (WI-0.093).
enum class Fixture : std::uint8_t { Hevc16, H264_32, Av1_64 };

inline constexpr std::array<std::string_view, 3> kFixtureFiles{"tiny16_hevc.fa", "tiny32_h264.fa", "tiny64_av1.fa"};

/// The stream hash every fixture's PlaneRef.blob_id names (fixtures_common.hpp, recon_blob_id).
inline constexpr std::string_view kFixtureSourceHash =
    "6f1d2c3b4a5968778695a4b3c2d1e0f0123456789abcdef0123456789abcdef0";

/// Unpacks @p fixture, applies @p fn and repacks it (canonical object-API build, identifier OVTA).
inline Bytes mutate(const Bytes& fixture, Mutator fn) {
  fb::FrameAnalysisT fa;
  fb::GetFrameAnalysis(fixture.data())->UnPackTo(&fa);
  fn(fa);
  flatbuffers::FlatBufferBuilder builder;
  fb::FinishFrameAnalysisBuffer(builder, fb::FrameAnalysis::Pack(builder, &fa));
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

/// Replaces the width of a struct-typed rect (structs have no mutators without flatc --gen-mutable).
inline void set_width(std::unique_ptr<fb::Rect>& rect, std::int32_t w) {
  rect = std::make_unique<fb::Rect>(rect->x(), rect->y(), w, rect->h());
}

/// One row of the table-driven rule test: applying @p mutate to @p fixture must make exactly @p rule fire.
struct RuleMutation {
  std::string_view rule;
  Fixture fixture;
  std::string_view what;
  Mutator mutate;
};

// Block ids used below. HEVC (§6.10): 0 root, 1 CTU, CUs 2/5/8/11, each followed by its PU and TU.
// H.264: 0 root, MBs 1/2/3/8, SubMBs 4..7 of MB 3 (8x8 each, from (0,16)). AV1: 0 root, 1 SB, Blocks + TXBs.
inline constexpr std::uint32_t kHevcCu = 2;
inline constexpr std::uint32_t kHevcPu = 3;
inline constexpr std::uint32_t kHevcTu = 4;
inline constexpr std::uint32_t kH264SubMb = 4;

inline const std::vector<RuleMutation>& rule_mutations() {
  static const std::vector<RuleMutation> rows{
      {"V-001", Fixture::Hevc16, "plugin_version empty", [](fb::FrameAnalysisT& fa) { fa.plugin_version.clear(); }},
      {"V-002", Fixture::Hevc16, "timebase_den 0", [](fb::FrameAnalysisT& fa) { fa.frame->timebase_den = 0; }},
      {"V-003", Fixture::Hevc16, "grid.intra_mode_c one element short",
       [](fb::FrameAnalysisT& fa) { fa.grid->intra_mode_c.pop_back(); }},
      {"V-004", Fixture::Hevc16, "root rect wider than coded_size",
       [](fb::FrameAnalysisT& fa) { set_width(fa.blocks[0]->rect, fa.blocks[0]->rect->w() + 16); }},
      {"V-005", Fixture::Hevc16, "PU listed twice in its CU's children",
       [](fb::FrameAnalysisT& fa) { fa.blocks[kHevcCu]->children.push_back(kHevcPu); }},
      {"V-006", Fixture::Hevc16, "TU width not a multiple of 4",
       [](fb::FrameAnalysisT& fa) { set_width(fa.blocks[kHevcTu]->rect, 6); }},
      {"V-007", Fixture::H264_32, "SubMB widened over its sibling",
       [](fb::FrameAnalysisT& fa) { set_width(fa.blocks[kH264SubMb]->rect, 16); }},
      {"V-008", Fixture::Hevc16, "TU turned into an AV1 TXB (no ext)",
       [](fb::FrameAnalysisT& fa) {
         auto& tu = *fa.blocks[kHevcTu];
         tu.kind = fb::BlockKind::TXB;
         tu.ext_type.clear();
         tu.ext.clear();
       }},
      {"V-009", Fixture::Hevc16, "TU level off by one", [](fb::FrameAnalysisT& fa) { fa.blocks[kHevcTu]->level = 4; }},
      {"V-010", Fixture::Hevc16, "cell 0 owned by the root",
       [](fb::FrameAnalysisT& fa) {
         fa.grid->block_id[0] = 0;
         fa.grid->bits_x8.clear();
       }},
      {"V-011", Fixture::Hevc16, "cell 0 owned by the CTU",
       [](fb::FrameAnalysisT& fa) {
         fa.grid->block_id[0] = 1;
         fa.grid->bits_x8.clear();
       }},
      {"V-012", Fixture::Hevc16, "cell 0 owned by the CU to its right",
       [](fb::FrameAnalysisT& fa) {
         fa.grid->block_id[0] = 5;
         fa.grid->bits_x8.clear();
       }},
  };
  return rows;
}

}  // namespace ovt::test

#endif  // OVT_TESTS_MODEL_MUTATE_H
