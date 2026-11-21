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
// ISOBMFF sample: the last OBU may omit obu_size), 2 = H.264 Annex B, 3 = H.265 Annex B. For AV1 (WI-1.124) bit 2
// turns selector 0 into an AV1 Annex B length-delimited stream (scan_obu_annexb, the SYN-AV1-0032 check) and bits
// 3-4 select the operating point (BuilderOptions.av1_operating_point).

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
  const unsigned selector = data[0] & 3U;
  const bool sample = selector == 1U;
  const bool annexb = selector == 0U && (data[0] & 4U) != 0;
  const std::span<const uint8_t> stream(data + 1, size - 1);
  const ovt::fb::Codec codec =
      selector == 2U ? ovt::fb::Codec::H264 : (selector == 3U ? ovt::fb::Codec::H265 : ovt::fb::Codec::AV1);
  ovt::index::BuilderOptions options{codec};
  options.av1_operating_point = static_cast<uint8_t>((data[0] >> 3U) & 3U);
  options.av1_annexb = annexb;
  ovt::index::StreamIndexBuilder builder(options);
  const ovt::index::SampleTiming timing{0, 0, 1, 30};
  const auto on_unit = [&builder, &timing, sample](const ovt::syntax::ScannedUnit& u) {
    ovt::index::UnitInput in;
    in.file_offset = u.file_offset;
    in.bytes = u.bytes;
    in.prefix_span = u.prefix_span;
    in.sample_index = u.container_sample_index;
    in.timing = sample ? &timing : nullptr;
    in.temporal_unit_start = u.temporal_unit_start;
    builder.add_unit(in);
  };
  const auto on_diag = [&builder](const ovt::syntax::ScanDiagnostic& d) { builder.add_scan_diagnostic(d); };
  if (annexb) {
    ovt::syntax::scan_obu_annexb(stream, 0, ovt::syntax::kNoContainerSample, on_unit, on_diag);
  } else if (codec == ovt::fb::Codec::AV1) {
    ovt::syntax::scan_obu_low_overhead(
        stream, 0, sample ? 0 : ovt::syntax::kNoContainerSample,
        sample ? ovt::syntax::ObuSpanKind::LengthDelimited : ovt::syntax::ObuSpanKind::Section5Stream, on_unit,
        on_diag);
  } else {
    ovt::syntax::scan_annexb(stream, 0, ovt::syntax::kNoContainerSample, on_unit, on_diag);
  }
  const std::array<uint8_t, 32> hash{};
  const flatbuffers::DetachedBuffer index = builder.finish(hash);
  if (!ovt::model::validate_stream_index({index.data(), index.size()}).empty()) {
    std::abort();
  }
  return 0;
}
