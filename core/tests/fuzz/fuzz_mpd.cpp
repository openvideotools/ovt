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
#include <optional>
#include <string>
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
    // A resolver derived from the input itself exercises the SegmentBase sidx path with hostile values.
    ovt::container::mpd::Options options{"http://example.invalid/dir/m.mpd", {}};
    options.sidx_resolver = [data, size](const std::string&, ovt::container::mpd::ByteRange) {
      ovt::container::mpd::SidxIndex idx;
      const auto word = [&](size_t at) {
        uint64_t v = 0;
        for (size_t i = 0; i < 8; ++i) {
          v = (v << 8U) | (size != 0 ? data[(at + i) % size] : 0U);
        }
        return v;
      };
      idx.timescale = static_cast<uint32_t>(word(0) | 1U);
      idx.earliest_presentation_time = word(8);
      idx.first_offset = word(16);
      for (size_t i = 0; i < 1 + (word(24) % 64U); ++i) {
        const uint64_t w = word(32 + (i * 8));
        idx.references.push_back({static_cast<uint32_t>(w), static_cast<uint32_t>(w >> 32U)});
      }
      return std::optional<ovt::container::mpd::SidxIndex>(std::move(idx));
    };
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
