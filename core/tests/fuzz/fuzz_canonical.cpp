// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of ovt::model::canonicalize and ovt::model::structural_diff (chapter 06 WI-0.085, chapter 14
// §14.7.1): both walk untrusted stored records (`ovt dump --diff A B`). The whole input is one FrameAnalysis
// buffer. Invariants, each an abort when violated: the only exception is std::invalid_argument (input that
// fails the verifier); when canonicalize accepts the input, its output is a fixed point (canonicalising it
// again gives the same bytes) and structurally equal to the input (empty diff both ways); a buffer is always
// structurally equal to itself.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <vector>

#include "ovt/cxx/model/canonical.hpp"
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
  std::vector<std::uint8_t> canonical;
  try {
    canonical = ovt::model::canonicalize(input, SchemaId::Model, kRoot);
  } catch (const std::invalid_argument&) {
    // Not a verifiable buffer (or a nested buffer that does not verify): the diff must reject or handle it.
    try {
      (void)ovt::model::structural_diff(input, input, SchemaId::Model, kRoot);
    } catch (const std::invalid_argument&) {
    }
    return 0;
  }
  require(ovt::model::canonicalize(canonical, SchemaId::Model, kRoot) == canonical);
  require(ovt::model::structural_diff(input, canonical, SchemaId::Model, kRoot).empty());
  require(ovt::model::structural_diff(canonical, input, SchemaId::Model, kRoot).empty());
  require(ovt::model::structural_diff(input, input, SchemaId::Model, kRoot).empty());
  return 0;
}
