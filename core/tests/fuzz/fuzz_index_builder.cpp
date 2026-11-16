// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the StreamIndex builder (chapter 7 §7.4.2; chapter 14 §14.7.1; WI-1.121). The first input byte
// selects the framing, the rest is the stream: the unit scanner splits it, every unit goes through
// StreamIndexBuilder::add_unit(), and finish() must return a buffer that ovt::model::validate_stream_index
// accepts (V-001, V-029, V-031) however broken the stream is. A crash, an exception out of the builder, a
// sanitizer report or a validator finding is a finding.
//
// Selector (low two bits): 0 = AV1 Section 5 stream (.obu), 1 = one AV1 length-delimited sample (IVF frame /
// ISOBMFF sample: the last OBU may omit obu_size). 2 and 3 are reserved for H.264 and H.265 Annex B (WI-1.121
// part b) and index as AV1 Section 5 until then.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_model_generated.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const bool sample = (data[0] & 3U) == 1U;
  const std::span<const uint8_t> stream(data + 1, size - 1);
  ovt::index::StreamIndexBuilder builder(ovt::index::BuilderOptions{ovt::fb::Codec::AV1});
  const ovt::index::SampleTiming timing{0, 0, 1, 30};
  ovt::syntax::scan_obu_low_overhead(
      stream, 0, sample ? 0 : ovt::syntax::kNoContainerSample,
      sample ? ovt::syntax::ObuSpanKind::LengthDelimited : ovt::syntax::ObuSpanKind::Section5Stream,
      [&builder, &timing, sample](const ovt::syntax::ScannedUnit& u) {
        ovt::index::UnitInput in;
        in.file_offset = u.file_offset;
        in.bytes = u.bytes;
        in.prefix_span = u.prefix_span;
        in.sample_index = u.container_sample_index;
        in.timing = sample ? &timing : nullptr;
        builder.add_unit(in);
      },
      [&builder](const ovt::syntax::ScanDiagnostic& d) { builder.add_scan_diagnostic(d); });
  const std::array<uint8_t, 32> hash{};
  const std::vector<uint8_t> index = builder.finish(hash);
  if (!ovt::model::validate_stream_index(index).empty()) {
    std::abort();
  }
  return 0;
}
