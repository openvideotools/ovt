// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// SampleTable expansion of one track's `stbl` (chapter 08 §8.4.1, WI-1.218). Internal header (not installed).
//
// Box payloads are read from the file, not from BoxNode fields: the sample-table decoders summarise large arrays
// (§8.2.5), so the tree is neither expanded nor modified. The fragment part of §8.4.1 is WI-1.219c.

#ifndef OVT_CONTAINER_ISOBMFF_SAMPLE_TABLE_H
#define OVT_CONTAINER_ISOBMFF_SAMPLE_TABLE_H

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "container/isobmff/box_walk.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// §8.4.1 step 1: a track longer than this is truncated (CNT-2031).
inline constexpr uint64_t kMaxSamples = 50'000'000;
// §8.4.1 step 6: CNT-2007 lists this many offending samples, then one summary line.
inline constexpr uint64_t kMaxListedOutOfFile = 16;

// SPEC-ASSUMPTION(#784): §8.4.1 names build_sample_table(ParsedMovie, ...); no ParsedMovie exists, so the entry points
// below take the file and the walked `trak` like build_tracks().
//
// The `trak` at ordinal `trak_index` (0-based, file order) of the first top-level `moov`; nullptr when there is none.
// This is the order of `build_tracks()`, so TrackInfo.index equals the ordinal even when two traks share a track_ID.
[[nodiscard]] const fb::BoxNodeT* find_trak(const BoxNodes& top_level, size_t trak_index);

// The moov part of the SampleTable of `trak` (§8.4.1 steps 1-9). Never throws for malformed input (every problem is a
// diagnostic on the table, and `count` is truncated where the spec says so); throws ovt::Cancelled when `cancel` is
// requested (checked every 64 Ki samples) and io::IoError on an I/O failure. Deterministic. A `trak` without `stbl`
// yields an empty table.
[[nodiscard]] std::unique_ptr<fb::SampleTableT> expand_sample_table(const io::RandomAccessFile& file,
                                                                    const fb::BoxNodeT& trak, jobs::Cancel& cancel);

// Samples [first, first + count) of `table` (count 0 = to the end; a range past the end is clamped) with the same
// header fields and diagnostics, for ovt_container_sample_table().
[[nodiscard]] std::unique_ptr<fb::SampleTableT> slice_sample_table(const fb::SampleTableT& table, uint64_t first,
                                                                   uint64_t count);

// The chapter-06 SampleTable FlatBuffer of a table.
[[nodiscard]] flatbuffers::DetachedBuffer serialise(const fb::SampleTableT& table);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_SAMPLE_TABLE_H
