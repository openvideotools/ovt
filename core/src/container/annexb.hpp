// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Annex B start-code splitter of H.264 and H.265 byte streams (chapter 08 §8.6.3, WI-1.226). It reports every
// NAL unit with its exact prefix_span and file_span and raises the CNT-ESB-* findings. Access-unit grouping is
// not done here (the index module does it); one "sample" of the container layer is one NAL unit.

#ifndef OVT_CONTAINER_ANNEXB_HPP
#define OVT_CONTAINER_ANNEXB_HPP

#include <cstdint>
#include <functional>
#include <span>

#include "container/format.hpp"
#include "container/split_types.hpp"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container {

// One NAL unit, as annexb_split hands it to the callback.
struct NalSpan {
  uint64_t index = 0;            // 0-based count of the NAL units reported so far (empty NAL units are not reported)
  FileSpan prefix_span;          // zero_byte? + 00 00 01 (3 or 4 bytes)
  FileSpan file_span;            // the NAL unit: header byte(s) to the last non-zero byte before the next start code
  uint8_t start_code_bytes = 3;  // 3 or 4
  uint8_t header_byte = 0;       // first byte of the NAL unit header (file_span is never empty)
};

using NalCallback = std::function<void(const NalSpan&)>;

struct AnnexBSummary {
  uint64_t nal_count = 0;
};

// Splits `file` in 1 MiB windows. `codec` (from sniff_detail().annexb_codec) only selects the NAL types that
// need a 4-byte start code (CNT-ESB-0004); None and Ambiguous skip that check.
// SPEC-ASSUMPTION(#278): CNT-ESB-0004 is raised for parameter sets and access unit delimiters only; "the first NAL
// unit of an access unit" needs slice header parsing, which belongs to the index module.
AnnexBSummary annexb_split(const io::RandomAccessFile& file, const NalCallback& on_nal,
                           const FindingSink& findings = {}, AnnexBCodec codec = AnnexBCodec::None);

// In-memory overload: `data[0]` is at file offset `base_offset`.
AnnexBSummary annexb_split(std::span<const uint8_t> data, uint64_t base_offset, const NalCallback& on_nal,
                           const FindingSink& findings = {}, AnnexBCodec codec = AnnexBCodec::None);

}  // namespace ovt::container

#endif  // OVT_CONTAINER_ANNEXB_HPP
