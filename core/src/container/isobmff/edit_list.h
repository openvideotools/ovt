// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The edit list presentation timeline of one track and the `elst ↔ tfdt` sanity check (chapter 08 §8.4.2, WI-1.220).
// Internal header (not installed).
//
// `elst`, `tkhd`, `mdhd` and `mvhd` are read from the file, not from BoxNode fields, like the SampleTable (§8.2.5).

#ifndef OVT_CONTAINER_ISOBMFF_EDIT_LIST_H
#define OVT_CONTAINER_ISOBMFF_EDIT_LIST_H

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/fragments.h"
#include "container/isobmff/sample_table.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// §8.4.2 step 3 bound: at most this many `elst` entries are kept.
inline constexpr uint32_t kMaxEditEntries = uint32_t{1} << 20U;

// The EditList of a track plus what the elst ↔ tfdt check needs about where it came from.
struct EditListBuild {
  std::unique_ptr<fb::EditListT> list;
  bool has_elst = false;  // the track has an `edts/elst` (otherwise `list` holds the implicit segment, step 1)
  uint32_t movie_timescale =
      0;                     // mvhd.timescale (the track timescale when there is no mvhd): the unit of segment_duration
  uint64_t diag_offset = 0;  // file offset of the `elst` (or of the `trak` without one): the span of CNT-3010..3012
};

// SPEC-ASSUMPTION(#793): §8.4.2 names build_edit_list(ParsedMovie, track_id, SampleTable); no ParsedMovie exists
// (#784), so this takes the file and the walked top level like build_tracks(), and the `trak` by ordinal like
// find_trak().
//
// Steps 1-4 of §8.4.2 for the `trak` at ordinal `trak_index` of the first `moov`, over the moov part of `table` (or the
// fragment-extended one, WI-1.219c). Findings CNT-3001..CNT-3004 become diagnostics of the list. A file without a
// `moov` or `trak` yields an empty list (track_id 0). Never throws for malformed input; throws ovt::Cancelled when
// `cancel` is requested (checked every 64 Ki samples) and io::IoError on an I/O failure. Deterministic.
[[nodiscard]] EditListBuild build_edit_list(const io::RandomAccessFile& file, const BoxNodes& top_level,
                                            size_t trak_index, const fb::SampleTableT& table, jobs::Cancel& cancel);

// Step 5: the presentation time of sample `sample_index` in the movie timescale (`movie_timescale`), computed with a
// 128-bit intermediate and truncated toward zero. False when the sample is in no `normal` segment (or `dwell` frozen
// sample), does not exist, or the time does not fit an int64.
[[nodiscard]] bool sample_presentation_time(const fb::EditListT& list, const fb::SampleTableT& table,
                                            uint64_t sample_index, uint32_t movie_timescale, int64_t* out_pts_movie);

// The `elst ↔ tfdt` sanity check (§8.4.2 rules 1-4) of a fragmented track: appends CNT-3010, CNT-3011 and CNT-3012 to
// `list.diagnostics`. `fragments` is the track's FragmentBuild (§8.4.3). Does nothing for a track without fragments.
// Idempotent per call site: call it once per list.
void check_elst_tfdt(EditListBuild& build, const FragmentBuild& fragments);

// The chapter-06 EditList FlatBuffer of a list.
[[nodiscard]] flatbuffers::DetachedBuffer serialise(const fb::EditListT& list);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_EDIT_LIST_H
