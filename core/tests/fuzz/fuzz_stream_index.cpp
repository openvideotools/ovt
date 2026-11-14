// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the StreamIndex readers (chapter 06 WI-0.094 step 3, chapter 14 §14.7.1): the input is a
// StreamIndex buffer, plain or size-prefixed, with or without the cache's "OVTI" identifier. It goes through
//   1. the generated Verifier with the validator's limits (max_depth 64, max_tables 10'000'000), and
//   2. ovt::model::validate_stream_index (V-001, V-029, V-031).
// Their verdicts must agree: a buffer the Verifier rejects yields exactly one violation, V-001.
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

const ovt::fb::StreamIndex* verify(const std::uint8_t* data, std::size_t size,
                                   const flatbuffers::Verifier::Options& opts) {
  {
    flatbuffers::Verifier verifier(data, size, opts);
    if (verifier.VerifyBuffer<ovt::fb::StreamIndex>(nullptr)) {
      return flatbuffers::GetRoot<ovt::fb::StreamIndex>(data);
    }
  }
  flatbuffers::Verifier verifier(data, size, opts);
  if (verifier.VerifySizePrefixedBuffer<ovt::fb::StreamIndex>(nullptr)) {
    return flatbuffers::GetSizePrefixedRoot<ovt::fb::StreamIndex>(data);
  }
  return nullptr;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> bytes(data, size);

  flatbuffers::Verifier::Options wide;
  wide.max_depth = 64;
  wide.max_tables = 10'000'000;
  const auto* root = size == 0 ? nullptr : verify(data, size, wide);

  const auto violations = ovt::model::validate_stream_index(bytes);
  if (root == nullptr) {
    require(violations.size() == 1U && violations[0].rule == "V-001");
    return 0;
  }
  for (const auto& v : violations) {
    require(v.rule.size() == 5U && v.rule.starts_with("V-0"));
  }
  return 0;
}
