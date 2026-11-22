// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The CENC per-sample encryption map and the clear / protected byte ranges of a sample (chapter 08 §8.4.4,
// WI-1.221). Internal header (not installed).
//
// Box payloads are read from the file, not from BoxNode fields: the decoders summarise large arrays (§8.2.5) and
// lazily walked moofs are header-only (§8.2.4), so the tree is neither expanded nor modified.

#ifndef OVT_CONTAINER_ISOBMFF_CENC_H
#define OVT_CONTAINER_ISOBMFF_CENC_H

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// Each finding code is listed at most this often per call; one summary line then says how many more there were.
inline constexpr uint64_t kMaxListedCencFindings = 16;

// SPEC-ASSUMPTION(#784): §8.4.4 names build_encryption_map(ParsedMovie, SampleTable, FragmentTimeline*,
// track_id, first, count); no ParsedMovie exists, so the entry point takes the walked top level and the `trak` ordinal
// like expand_sample_table().
//
// The EncryptionInfo of the track at `trak_ordinal` (0-based, file order, as find_trak()) with one SampleEncryption per
// sample of [first, first + count) of `table` (the track's SampleTable; count 0 = to the end; a range past the end is
// clamped). Only the samples of the moov part are covered (aux data of `stbl`, WI-1.221a); the fragment samples
// follow in WI-1.221b. A track without encryption signalling yields an EncryptionInfo with no scheme and no samples.
// Never throws for malformed input (every problem is a diagnostic on the result); throws ovt::Cancelled when `cancel`
// is requested (checked every 4096 samples) and io::IoError on an I/O failure. Deterministic.
[[nodiscard]] std::unique_ptr<fb::EncryptionInfoT> build_encryption_map(const io::RandomAccessFile& file,
                                                                        const BoxNodes& top_level, size_t trak_ordinal,
                                                                        const fb::SampleTableT& table, uint64_t first,
                                                                        uint64_t count, jobs::Cancel& cancel);

// The absolute byte ranges of a sample, alternating clear, protected, clear, ... (even indices are clear, odd are
// protected; a range may be empty, and adjacent ranges of one kind are merged). A sample that is not protected is one
// clear range. A protected sample without subsamples is one protected range over the whole sample. For the pattern
// schemes (`cens`, `cbcs`: crypt_byte_block:skip_byte_block) the pattern restarts at each subsample's protected range
// and only the crypt blocks of it are protected; a partial trailing block (< 16 bytes) is clear for `cbc1` and `cbcs`
// and follows the pattern for `cenc` and `cens` (SPEC-ASSUMPTION(#808)).
[[nodiscard]] std::vector<syntax::ByteRange> sample_clear_protected_ranges(const fb::SampleEncryptionT& sample,
                                                                           uint64_t sample_offset,
                                                                           uint32_t sample_size);

// The chapter-06 EncryptionInfo FlatBuffer.
[[nodiscard]] flatbuffers::DetachedBuffer serialise(const fb::EncryptionInfoT& info);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_CENC_H
