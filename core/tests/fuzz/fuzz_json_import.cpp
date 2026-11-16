// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of ovt::model::from_json (chapter 06 WI-0.088, chapter 14 §14.7.1): the JSON importer behind
// `ovt cache import <file.json>` reads untrusted text. The whole input is the text of one FrameAnalysis.
// Invariants, each an abort when violated: the only exception is std::invalid_argument; an accepted text
// gives a canonical buffer (canonicalising it again changes nothing) that to_json exports (a string that is
// not UTF-8 cannot get here: the parser rejects it), and importing that export gives the same text again.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  std::vector<std::uint8_t> buffer;
  try {
    buffer = ovt::model::from_json(text, SchemaId::Model, kRoot);
  } catch (const std::invalid_argument&) {
    return 0;
  }
  require(ovt::model::canonicalize(buffer, SchemaId::Model, kRoot) == buffer);
  const std::string exported = ovt::model::to_json(buffer, SchemaId::Model, kRoot, {.compact = true});
  const auto again = ovt::model::from_json(exported, SchemaId::Model, kRoot);
  require(ovt::model::to_json(again, SchemaId::Model, kRoot, {.compact = true}) == exported);
  return 0;
}
