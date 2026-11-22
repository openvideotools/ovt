// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The fragment timeline of one track (chapter 08 §8.4.3 steps 1-5; WI-1.219a, sidx / mfra cross-checks WI-1.219b).
// The SampleTable append and ovt_container_fragments are WI-1.219c. Internal header (not installed).
//
// Box payloads are read from the file, not from BoxNode fields: the trun decoder summarises large arrays (§8.2.5) and
// lazily walked moofs are header-only (§8.2.4), so the tree is neither expanded nor modified.

#ifndef OVT_CONTAINER_ISOBMFF_FRAGMENTS_H
#define OVT_CONTAINER_ISOBMFF_FRAGMENTS_H

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// §8.4.1 step 1 bound, shared by the fragment part: at most this many fragment samples are expanded (CNT-2031).
inline constexpr uint64_t kMaxFragmentSamples = 50'000'000;

// The `trex` defaults of one track (14496-12 §8.8.3).
struct TrexDefaults {
  uint32_t sample_description_index = 0;
  uint32_t duration = 0;
  uint32_t size = 0;
  uint32_t flags = 0;
};

// What the builder needs from `moov`. WI-1.219c fills it from ParsedMovie / TrackInfo; fragment_track_input()
// reads it from the box tree until then.
struct FragmentTrackInput {
  uint32_t track_id = 0;
  uint32_t timescale = 0;         // mdhd.timescale, 0 = unknown (no CNT-4031 / CNT-4032 then)
  bool has_moov_samples = false;  // the track's stbl holds samples: the first fragment continues them (§8.4.3 2.5)
  uint64_t moov_duration = 0;     // Σ stts sample_count × sample_delta of the track
  std::map<uint32_t, TrexDefaults> trex;       // every `moov/mvex/trex`, by track_ID (other tracks' sizes chain bases)
  uint64_t max_samples = kMaxFragmentSamples;  // expansion bound (CNT-2031); the fuzz target lowers it
};

// The fragment samples of the track in file order, structure of arrays (what §8.4.1 "fragment part" appends to the
// SampleTable). `flags` is the effective sample_flags word (first_sample_flags / trun / tfhd / trex).
struct FragmentSamples {
  std::vector<int64_t> dts;
  std::vector<int32_t> cts_offset;
  std::vector<uint32_t> duration;
  std::vector<uint64_t> offset;
  std::vector<uint32_t> size;
  std::vector<uint32_t> flags;
  std::vector<uint32_t> sample_description_index;
  std::vector<int32_t> fragment_index;  // index into FragmentTimeline.fragments

  [[nodiscard]] size_t count() const noexcept { return dts.size(); }
  // 14496-12 §8.8.3.1: sample_is_non_sync_sample is bit 16 of sample_flags.
  [[nodiscard]] bool is_sync(size_t i) const noexcept { return (flags[i] & 0x10000U) == 0; }
};

struct FragmentBuild {
  fb::FragmentTimelineT timeline;
  FragmentSamples samples;
};

// Reads the FragmentTrackInput of `track_id` from the `moov` of the walked tree (tkhd, mdhd, stts, stsz, trex).
// A file without `moov` (fragment-only segment) yields the track_id and nothing else.
[[nodiscard]] FragmentTrackInput fragment_track_input(const io::RandomAccessFile& file, const BoxNodes& top_level,
                                                      uint32_t track_id);

// §8.4.3 steps 1-5 for one track over the top-level boxes of the walk (file order). Never throws for malformed
// input (all problems become diagnostics on the timeline); throws ovt::Cancelled when `cancel` is requested (checked
// per `moof` and every 64 Ki samples) and io::IoError on an I/O failure. Deterministic.
[[nodiscard]] FragmentBuild build_fragments(const io::RandomAccessFile& file, const BoxNodes& top_level,
                                            const FragmentTrackInput& input, jobs::Cancel& cancel);

// The chapter-06 FragmentTimeline FlatBuffer of a built timeline.
[[nodiscard]] flatbuffers::DetachedBuffer serialise(const fb::FragmentTimelineT& timeline);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_FRAGMENTS_H
