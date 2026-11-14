// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the raw AV1 OBU detection and walk (chapter 08 §8.6.4 and §8.9.4, WI-1.227). Byte 0 selects the
// framing to walk (0 Section 5, 1 Annex B, 2 the detected one), the rest is the file. Invariants, each an abort
// when violated: no exception escapes; the walk is deterministic; temporal units are in order, non-overlapping and
// inside the input; every OBU lies inside the input and inside its temporal unit; the summary counts equal the
// callbacks; a Section 5 walk reports temporal_unit_count == its number of temporal delimiters (plus one when the
// stream starts with another OBU); re-framing an OBU either fails or gives a valid sized OBU of the right length.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "container/obu.hpp"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace ovt::container;
  if (size < 1) {
    return 0;
  }
  const std::span<const uint8_t> bytes(data + 1, size - 1);
  try {
    const auto detected = obu_detect(bytes.first(std::min<size_t>(bytes.size(), 65536)), bytes.size());
    ObuFraming framing = data[0] % 3 == 0 ? ObuFraming::Section5 : ObuFraming::AnnexB;
    if (data[0] % 3 == 2) {
      framing = detected.framing;
    }

    std::vector<ObuNode> obus;
    std::vector<TemporalUnitNode> tus;
    ObuVisitor v;
    v.on_obu = [&](const ObuNode& n) { obus.push_back(n); };
    v.on_temporal_unit = [&](const TemporalUnitNode& n) { tus.push_back(n); };
    const auto summary = obu_walk(bytes, 0, framing, v);
    require(summary.obu_count == obus.size() && summary.temporal_unit_count == tus.size());

    uint64_t floor = 0;
    for (const auto& tu : tus) {
      require(tu.file_span.offset >= floor && tu.file_span.end() <= bytes.size());
      floor = tu.file_span.end();
    }
    uint64_t delimiters = 0;
    uint64_t last_end = 0;
    for (const auto& n : obus) {
      require(n.file_span.length >= n.header.header_bytes && n.file_span.end() <= bytes.size());
      require(n.file_span.offset >= last_end);
      last_end = n.file_span.end();
      require(n.temporal_unit_index < tus.size());
      const auto& tu = tus[n.temporal_unit_index];
      require(n.file_span.offset >= tu.file_span.offset && n.file_span.end() <= tu.file_span.end());
      delimiters += n.header.obu_type == kObuTemporalDelimiter ? 1 : 0;
      if (framing == ObuFraming::AnnexB) {
        std::vector<uint8_t> out;
        const auto obu_bytes = bytes.subspan(n.file_span.offset, n.file_span.length);
        if (reframe_annexb_obu(obu_bytes, out)) {
          const auto h = parse_obu_header(out);
          require(h.valid && h.has_size_field);
          const auto leb = read_leb128(std::span<const uint8_t>(out).subspan(h.header_bytes));
          require(leb.status == LebStatus::Ok && h.header_bytes + leb.bytes + leb.value == out.size());
        }
      }
    }
    if (framing == ObuFraming::Section5 && !obus.empty()) {
      const bool starts_without_td = obus.front().header.obu_type != kObuTemporalDelimiter;
      require(summary.temporal_unit_count == delimiters + (starts_without_td ? 1 : 0));
    }

    // Deterministic.
    std::vector<ObuNode> again;
    ObuVisitor v2;
    v2.on_obu = [&](const ObuNode& n) { again.push_back(n); };
    obu_walk(bytes, 0, framing, v2);
    require(again.size() == obus.size());

    // The same bytes at a shifted base offset give the same walk, shifted.
    constexpr uint64_t kShift = 777;
    std::vector<ObuNode> shifted;
    ObuVisitor v3;
    v3.on_obu = [&](const ObuNode& n) { shifted.push_back(n); };
    obu_walk(bytes, kShift, framing, v3);
    require(shifted.size() == obus.size());
    for (size_t i = 0; i < obus.size(); ++i) {
      require(shifted[i].file_span.offset == obus[i].file_span.offset + kShift &&
              shifted[i].file_span.length == obus[i].file_span.length);
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
