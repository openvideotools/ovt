// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The HdrMetadata summary of one ISOBMFF track (chapter 08 §8.4.6, WI-1.223): the box-level pass over the first
// sample entry of the track (with the parameter sets of its configuration record), the classification rules 3.1-3.7,
// the box-vs-bitstream mismatch rules and the 44-byte ovt_ColorDescription image. The bounded sync-sample probe
// (step 2, HdrProbeOptions) is WI-1.223b; until then probed_samples is 0.

#ifndef OVT_CONTAINER_ISOBMFF_HDR_SUMMARY_H
#define OVT_CONTAINER_ISOBMFF_HDR_SUMMARY_H

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <vector>

#include "ovt/color.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// Builds the HdrMetadata of the track whose `trak` box is `trak` (walked tree, object API). Never throws for
// malformed bytes: a box that does not parse is left out and the summary keeps the rest. An I/O failure of `file`
// (IoError) propagates.
// SPEC-ASSUMPTION(#770): §8.4.6 takes `const ParsedMovie&` and a SampleTable; ParsedMovie arrives with WI-1.217 and
// the SampleTable with WI-1.218, so this entry point takes the trak node the ParsedMovie overload will look up.
[[nodiscard]] fb::HdrMetadataT summarize_hdr(const fb::BoxNodeT& trak, uint32_t track_id,
                                             const io::RandomAccessFile& file);

// summarize_hdr() serialised as an HdrMetadata root buffer.
[[nodiscard]] flatbuffers::DetachedBuffer build_hdr_metadata(const fb::BoxNodeT& trak, uint32_t track_id,
                                                             const io::RandomAccessFile& file);

// HdrMetadata.color as an ovt_ColorDescription (little-endian image, struct_size first; §0.5.6, D4). An image of
// another size gives a zeroed struct with struct_size set.
[[nodiscard]] ovt_ColorDescription color_from_image(const std::vector<uint8_t>& image) noexcept;

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_HDR_SUMMARY_H
