// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of ovt::model::to_json (chapter 06 WI-0.088, chapter 14 §14.7.1): the JSON exporter walks
// stored records, which are untrusted (`--format json` over a cache entry or a plugin's buffer). The whole
// input is one FrameAnalysis buffer. Invariants, each an abort when violated: the only exception is
// std::invalid_argument (a buffer that fails the verifier, a string that is not UTF-8, work past the budget);
// both layouts are accepted or rejected alike and the export is deterministic; the canonical form of the
// input (when canonicalize accepts it) exports too, with the text the input gives once its present but empty
// strings and vectors, which canonical form drops, are set aside (checked on inputs that have none).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>

#include "ovt/cxx/model/canonical.hpp"
#include "ovt/cxx/model/json.hpp"
#include "ovt/cxx/model/schema_id.hpp"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

constexpr const char* kRoot = "ovt.fb.FrameAnalysis";

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using ovt::model::SchemaId;
  const std::span<const std::uint8_t> input(data, size);
  std::string pretty;
  try {
    pretty = ovt::model::to_json(input, SchemaId::Model, kRoot);
  } catch (const std::invalid_argument&) {
    return 0;
  }
  const std::string compact =
      ovt::model::to_json(input, SchemaId::Model, kRoot, {.compact = true, .strict_numbers = true});
  require(compact.find('\n') == std::string::npos);
  require(ovt::model::to_json(input, SchemaId::Model, kRoot) == pretty);
  try {
    const auto canonical = ovt::model::canonicalize(input, SchemaId::Model, kRoot);
    const std::string again = ovt::model::to_json(canonical, SchemaId::Model, kRoot);
    const bool has_empty = pretty.find(R"(": "")") != std::string::npos || pretty.find(R"(": [])") != std::string::npos;
    require(has_empty || again == pretty);
  } catch (const std::invalid_argument&) {  // canonicalize charges its work differently and may give up first
  }
  return 0;
}
