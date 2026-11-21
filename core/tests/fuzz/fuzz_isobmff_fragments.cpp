// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the FragmentTimeline builder (chapter 08 §8.4.3, WI-1.219). The whole input is the file: the box walk
// with the lazy moof policy, then fragment_track_input() and build_fragments() for track_IDs 1, 2 and the value of the
// first input byte. Invariants, each an abort when violated: no exception escapes (I/O on a memory file cannot fail);
// every sample array has the same length, at most the lowered expansion bound; every sample's fragment_index names a
// fragment; the serialised timeline verifies.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/fragments.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt_container_generated.h"

namespace {

namespace iso = ovt::container::isobmff;

constexpr uint64_t kMaxSamples = 1U << 16U;  // keeps one input fast; the cap itself is unit-tested

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

void check(const iso::FragmentBuild& b) {
  const iso::FragmentSamples& s = b.samples;
  const size_t n = s.count();
  require(n <= kMaxSamples);
  require(s.cts_offset.size() == n && s.duration.size() == n && s.offset.size() == n && s.size.size() == n &&
          s.flags.size() == n && s.sample_description_index.size() == n && s.fragment_index.size() == n);
  for (const int32_t f : s.fragment_index) {
    require(f >= 0 && static_cast<size_t>(f) < b.timeline.fragments.size());
  }
  const flatbuffers::DetachedBuffer buf = iso::serialise(b.timeline);
  flatbuffers::Verifier v(buf.data(), buf.size());
  require(v.VerifyBuffer<ovt::fb::FragmentTimeline>(nullptr));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> bytes(data, size);
  const ovt::io::MemoryFile file(bytes);
  std::vector<iso::DeferredBox> deferred;
  const iso::WalkResult w = iso::walk(file, &deferred);
  const std::vector<uint32_t> tracks = {1, 2, bytes.empty() ? 0U : bytes[0]};
  for (const uint32_t id : tracks) {
    iso::FragmentTrackInput in = iso::fragment_track_input(file, w.top_level, id);
    in.max_samples = kMaxSamples;  // a 20-byte trun may claim 2^32 default-sized samples
    check(iso::build_fragments(file, w.top_level, in, ovt::jobs::Cancel::never()));
  }
  return 0;
}
