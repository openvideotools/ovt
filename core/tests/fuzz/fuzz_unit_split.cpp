// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the unit splitter (chapter 08 §8.7.4, WI-1.231). Byte 0 selects the framing (bits 0-1) and, for
// length-prefixed samples, the length size (bits 2-3: 1, 2, 3 (invalid) or 4); bit 4 sets
// temporal_delimiters_expected. The rest is the sample. Invariants, each an abort when violated: no exception
// escapes; the split is deterministic; units are non-empty, in order, non-overlapping and inside the sample with
// their prefix; a rewritten split is AV1 and its last unit's bytes are a valid sized OBU of the right length; every
// diagnostic carries a registered CNT code.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "container/diagnostics.h"
#include "container/obu.hpp"
#include "demux/unit_split.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace ovt::demux;
  if (size < 1) {
    return 0;
  }
  const std::span<const uint8_t> sample(data + 1, size - 1);
  const auto framing = static_cast<Framing>(data[0] & 3U);
  static constexpr uint8_t kLengthSizes[] = {1, 2, 3, 4};
  const uint8_t length_size = kLengthSizes[(data[0] >> 2U) & 3U];
  const bool td_expected = (data[0] & 0x10U) != 0;
  try {
    std::vector<uint8_t> scratch;
    const SplitResult r = split_units(sample, framing, length_size, scratch, td_expected);
    std::vector<uint8_t> scratch2;
    const SplitResult again = split_units(sample, framing, length_size, scratch2, td_expected);
    require(again.units == r.units && scratch2 == scratch && again.rewritten == r.rewritten &&
            again.diagnostics.size() == r.diagnostics.size());

    uint64_t previous_end = 0;
    for (std::size_t i = 0; i < r.units.size(); ++i) {
      const UnitSpan& u = r.units[i];
      require(u.length > 0);
      require(u.offset_in_packet >= u.prefix_length);
      require(u.offset_in_packet - u.prefix_length >= previous_end);
      require(u.offset_in_packet + u.length <= sample.size());
      previous_end = u.offset_in_packet + u.length;
      const auto bytes = unit_bytes(sample, r, scratch, i);
      require(!bytes.empty());
    }
    if (r.rewritten) {
      require(framing == Framing::Av1Obu && !r.units.empty());
      const auto last = unit_bytes(sample, r, scratch, r.units.size() - 1);
      const auto h = ovt::container::parse_obu_header(last.first(std::min<std::size_t>(2, last.size())));
      require(h.valid && h.has_size_field);
      const auto obu_size = ovt::container::read_leb128(last.subspan(h.header_bytes));
      require(obu_size.status == ovt::container::LebStatus::Ok);
      require(h.header_bytes + obu_size.bytes + obu_size.value == last.size());
      require(obu_size.value + h.header_bytes == r.units.back().length);
    }
    for (const auto& d : r.diagnostics) {
      require(ovt::container::diag(d.code) != nullptr);
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
