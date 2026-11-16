// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the HLS M3U8 parser (chapter 8 §8.10, WI-4.209). The whole input is the playlist text. Invariants,
// each an abort when violated: no exception escapes; parsing is deterministic; the segment list never exceeds the
// cap; segment start times never decrease; the Manifest table built from the result verifies.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "container/virtual_file/m3u8.hpp"
#include "ovt_container_generated.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const ovt::container::m3u8::Options options{"http://example.invalid/dir/p.m3u8"};
    const auto a = ovt::container::m3u8::parse(text, options);
    const auto b = ovt::container::m3u8::parse(text, options);
    require(a.ok == b.ok);
    require(a.manifest.media.segments.size() == b.manifest.media.segments.size());
    require(a.manifest.variants.size() == b.manifest.variants.size());
    require(a.manifest.diagnostics.size() == b.manifest.diagnostics.size());
    require(a.manifest.media.segments.size() <= 1'000'000);
    int64_t last_start = -1;
    for (const auto& s : a.manifest.media.segments) {
      if (!s.is_init) {
        require(s.start_time >= last_start);
        last_start = s.start_time;
      }
    }
    if (a.ok && a.manifest.media.segments.size() <= 5000) {
      flatbuffers::FlatBufferBuilder fbb;
      fbb.Finish(ovt::container::m3u8::write_manifest(fbb, a.manifest));
      flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
      require(verifier.VerifyBuffer<ovt::fb::Manifest>(nullptr));
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
