// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of ovt::rbsp_from_nal and the raw <-> RBSP offset mapping (chapter 7 §7.2.5, chapter 14 §14.7.1,
// WI-1.105). The whole input is one NAL unit (capped at 64 KiB by the engine's -max_len, see the fuzz-smoke
// workflow). Invariants, each an abort when violated: the EPB list is strictly ascending and points at 0x03
// bytes; re-inserting the removed bytes reproduces the input exactly; the two mapping functions are inverse
// to each other on every RBSP byte and agree with the byte values; no exception escapes. The mapping
// functions are also called with out-of-range arguments, where they must neither crash nor throw.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "ovt/cxx/rbsp.hpp"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

// Reference inverse of rbsp_from_nal: put a 0x03 back at every recorded raw position.
std::vector<uint8_t> reinsert(const ovt::Rbsp& r) {
  std::vector<uint8_t> raw;
  raw.reserve(r.rbsp.size() + r.epb_positions.size());
  size_t src = 0;
  for (const uint32_t pos : r.epb_positions) {
    while (raw.size() < pos) {
      require(src < r.rbsp.size());
      raw.push_back(r.rbsp[src++]);
    }
    raw.push_back(0x03);
  }
  raw.insert(raw.end(), r.rbsp.begin() + static_cast<std::ptrdiff_t>(src), r.rbsp.end());
  return raw;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> nal(data, size);
  const ovt::Rbsp r = ovt::rbsp_from_nal(nal);

  require(r.rbsp.size() + r.epb_positions.size() == size);
  uint32_t prev = 0;
  for (size_t j = 0; j < r.epb_positions.size(); ++j) {
    const uint32_t p = r.epb_positions[j];
    require(p < size && nal[p] == 0x03);
    require(j == 0 || p > prev);
    require(p >= 2);  // an EPB follows two zero bytes of the unit
    prev = p;
  }
  require(std::ranges::equal(reinsert(r), nal));

  for (size_t b = 0; b < r.rbsp.size(); ++b) {
    const uint64_t raw_bit = ovt::rbsp_bit_to_raw_bit((b * 8) + 5, r.epb_positions);
    require(raw_bit % 8 == 5);
    const uint64_t raw = raw_bit / 8;
    require(raw < size && nal[raw] == r.rbsp[b]);
    require(ovt::raw_byte_to_rbsp_byte(raw, r.epb_positions) == std::pair<uint64_t, bool>(b, false));
  }
  for (const uint32_t p : r.epb_positions) {
    require(ovt::raw_byte_to_rbsp_byte(p, r.epb_positions).second);
  }

  // Out-of-range arguments are defined (the position just keeps shifting); they must not throw or crash.
  const uint64_t far = size == 0 ? 0 : (uint64_t{data[0]} << 40) | (uint64_t{data[size - 1]} << 3);
  (void)ovt::rbsp_bit_to_raw_bit(far, r.epb_positions);
  (void)ovt::raw_byte_to_rbsp_byte(far, r.epb_positions);
  (void)ovt::rbsp_bit_to_raw_bit(UINT64_MAX, r.epb_positions);
  (void)ovt::raw_byte_to_rbsp_byte(UINT64_MAX, r.epb_positions);
  return 0;
}
