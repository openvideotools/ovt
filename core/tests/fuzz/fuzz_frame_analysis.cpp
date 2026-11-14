// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the FrameAnalysis readers (chapter 06 WI-0.094 step 3, chapter 14 §14.7.1): the input is a
// FrameAnalysis buffer, plain or size-prefixed, as a plugin or a cache file hands it over. It goes through
//   1. the generated Verifier with the validator's limits (max_depth 64, max_tables 10'000'000), and
//   2. ovt::model::validate_frame_analysis (every rule of §6.9; V-001 is the Verifier again), and
//   3. for a buffer that verifies under a tighter table budget, the object API: UnPack, Pack, and a second
//      verification of the repacked bytes, which must pass (a verified buffer can always be rebuilt).
// A rejected buffer is a normal outcome. A crash, a sanitizer report, an exception or a timeout is a finding.
// SPEC-ASSUMPTION(#435): `to_json` (WI-0.088) is not merged yet; it joins step 3 with that work item.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "ovt/cxx/model/validator.hpp"
#include "ovt_model_generated.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

// The object API expands every shared sub-table (a hostile buffer can point many offsets at one table), so
// the rebuild runs only for buffers within this table budget; the Verifier counts every visit.
constexpr flatbuffers::uoffset_t kRepackMaxTables = 100'000;

const ovt::fb::FrameAnalysis* verify(const std::uint8_t* data, std::size_t size,
                                     const flatbuffers::Verifier::Options& opts) {
  {
    flatbuffers::Verifier verifier(data, size, opts);
    if (ovt::fb::VerifyFrameAnalysisBuffer(verifier)) {
      return ovt::fb::GetFrameAnalysis(data);
    }
  }
  flatbuffers::Verifier verifier(data, size, opts);
  if (ovt::fb::VerifySizePrefixedFrameAnalysisBuffer(verifier)) {
    return ovt::fb::GetSizePrefixedFrameAnalysis(data);
  }
  return nullptr;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> bytes(data, size);

  flatbuffers::Verifier::Options wide;
  wide.max_depth = 64;
  wide.max_tables = 10'000'000;
  const auto* root = verify(data, size, wide);

  const auto violations = ovt::model::validate_frame_analysis(bytes);
  if (root == nullptr) {
    // The validator's V-001 is the same Verifier: a rejected buffer yields that single violation.
    require(violations.size() == 1U && violations[0].rule == "V-001");
    return 0;
  }
  for (const auto& v : violations) {
    require(v.rule.size() == 5U && v.rule.starts_with("V-0"));
  }

  flatbuffers::Verifier::Options tight = wide;
  tight.max_tables = kRepackMaxTables;
  root = verify(data, size, tight);
  if (root == nullptr) {
    return 0;
  }
  ovt::fb::FrameAnalysisT native;
  root->UnPackTo(&native);
  flatbuffers::FlatBufferBuilder builder;
  ovt::fb::FinishFrameAnalysisBuffer(builder, ovt::fb::FrameAnalysis::Pack(builder, &native));
  flatbuffers::Verifier again(builder.GetBufferPointer(), builder.GetSize(), wide);
  require(ovt::fb::VerifyFrameAnalysisBuffer(again));
  return 0;
}
