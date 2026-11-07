// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// rbsp_from_nal and the raw <-> RBSP offset mapping (chapter 7 §7.2.5, WI-1.104).

#include "ovt/cxx/rbsp.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ovt {

Rbsp rbsp_from_nal(std::span<const uint8_t> nal_bytes) {
  // SPEC-ASSUMPTION(#76): epb_positions are 32-bit, so a NAL over 4 GiB is refused.
  if (nal_bytes.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("rbsp_from_nal: NAL unit larger than 4 GiB");
  }
  Rbsp out;
  out.rbsp.reserve(nal_bytes.size());
  // §7.2.5 loop, exactly. The EPB is removed regardless of the following byte (a following byte > 0x03,
  // or none at all for cabac_zero_word padding, is still an EPB).
  // SPEC-ASSUMPTION(#76): SYN-GEN-0002 ("EPB followed by > 0x03") is not reported here; Rbsp has no
  // diagnostics channel. The syntax layer derives it from nal_bytes[epb_positions[i] + 1].
  unsigned zeros = 0;
  for (size_t i = 0; i < nal_bytes.size(); ++i) {
    const uint8_t b = nal_bytes[i];
    if (zeros >= 2 && b == 0x03) {
      out.epb_positions.push_back(static_cast<uint32_t>(i));
      zeros = 0;
      continue;
    }
    out.rbsp.push_back(b);
    zeros = (b == 0x00) ? zeros + 1 : 0;
  }
  return out;
}

uint64_t rbsp_bit_to_raw_bit(uint64_t rbsp_bit, std::span<const uint32_t> epb_positions) {
  // The j-th EPB (0-based) precedes the raw byte of RBSP byte r iff epb_positions[j] - j <= r: before it
  // there are j removed bytes, so it sits just before RBSP byte epb_positions[j] - j. That key is
  // non-decreasing in j (positions are strictly ascending), so the number k of EPBs that precede RBSP byte
  // r is one binary search; raw byte = r + k. This is the §7.2.5 "incremental frontier" in O(log n).
  const uint64_t rbsp_byte = rbsp_bit / 8;
  size_t lo = 0;  // invariant: every j < lo precedes, every j >= hi does not
  size_t hi = epb_positions.size();
  while (lo < hi) {
    const size_t mid = lo + ((hi - lo) / 2);
    if (uint64_t{epb_positions[mid]} - mid <= rbsp_byte) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  const uint64_t k = lo;
  return ((rbsp_byte + k) * 8) + (rbsp_bit % 8);
}

std::pair<uint64_t, bool> raw_byte_to_rbsp_byte(uint64_t raw_byte, std::span<const uint32_t> epb_positions) {
  // k = count(epb_positions < raw_byte); the raw byte is an EPB iff the next position equals it.
  const auto it = std::lower_bound(epb_positions.begin(), epb_positions.end(), raw_byte,
                                   [](uint32_t p, uint64_t v) { return uint64_t{p} < v; });
  const auto k = static_cast<uint64_t>(it - epb_positions.begin());
  const bool is_epb = it != epb_positions.end() && uint64_t{*it} == raw_byte;
  return {raw_byte - k, is_epb};
}

}  // namespace ovt
