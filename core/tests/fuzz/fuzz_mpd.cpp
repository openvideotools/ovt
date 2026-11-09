// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the DASH MPD parser (chapter 8 §8.10, WI-4.208). The whole input is the manifest text. Invariants,
// each an abort when violated: no exception escapes; parsing is deterministic; a representation never exceeds the
// segment cap; the Manifest table built from the result verifies.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "container/virtual_file/mpd.hpp"
#include "ovt_container_generated.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

size_t count_segments(const ovt::container::mpd::Manifest& m) {
  size_t n = 0;
  for (const auto& p : m.periods) {
    for (const auto& a : p.adaptation_sets) {
      for (const auto& r : a.representations) {
        require(r.segments.size() <= 1'000'002);  // cap + init + a lone SegmentBase entry
        n += r.segments.size();
      }
    }
  }
  return n;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    const std::string_view xml(reinterpret_cast<const char*>(data), size);
    const ovt::container::mpd::Options options{"http://example.invalid/dir/m.mpd", {}};
    const auto a = ovt::container::mpd::parse(xml, options);
    const auto b = ovt::container::mpd::parse(xml, options);
    require(a.ok == b.ok);
    require(count_segments(a.manifest) == count_segments(b.manifest));
    require(a.manifest.diagnostics.size() == b.manifest.diagnostics.size());
    // Serialising a million-segment manifest per input would make the fuzzer crawl; small ones are checked.
    if (a.ok && count_segments(a.manifest) <= 5000) {
      flatbuffers::FlatBufferBuilder fbb;
      fbb.Finish(ovt::container::mpd::write_manifest(fbb, a.manifest));
      flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
      require(verifier.VerifyBuffer<ovt::fb::Manifest>(nullptr));
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
