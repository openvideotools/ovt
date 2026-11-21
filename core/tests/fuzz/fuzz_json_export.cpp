// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of ovt::model::to_json (chapter 06 WI-0.088, chapter 14 §14.7.1): the JSON exporter walks
// stored records, which are untrusted (`--format json` over a cache entry or a plugin's buffer). The whole
// input is one FrameAnalysis buffer. Invariants, each an abort when violated: the only exception is
// std::invalid_argument (a buffer that fails the verifier, a string that is not UTF-8, work past the budget);
// both layouts are accepted or rejected alike; the compact layout has no newline; the export is deterministic;
// the canonical input exports to the same text unless the input has a present but empty string or vector;
// when the importer accepts the export (it may refuse tables
// nested deeper than its parser allows), the result is structurally equal to the input (FrameAnalysis has no
// floating-point field, so the round trip is exact) and exports to the same text as the canonical input.

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
  try {
    const auto imported = ovt::model::from_json(pretty, SchemaId::Model, kRoot);
    require(ovt::model::from_json(compact, SchemaId::Model, kRoot) == imported);
    require(ovt::model::structural_diff(input, imported, SchemaId::Model, kRoot).empty());
    // The import is canonical: a present but empty string or vector of the input is gone, nothing else.
    const auto canonical = ovt::model::canonicalize(input, SchemaId::Model, kRoot);
    require(ovt::model::to_json(imported, SchemaId::Model, kRoot) ==
            ovt::model::to_json(canonical, SchemaId::Model, kRoot));
  } catch (const std::invalid_argument&) {
  }
  return 0;
}
