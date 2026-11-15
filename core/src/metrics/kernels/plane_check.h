// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Input checks shared by the in-tree integer-domain kernels (ssim_x264; WI-3.308a, identity follows in WI-3.308b): what
// the host may hand to score() for a code-value metric, and what makes a reference/distorted pair comparable.

#ifndef OVT_SRC_METRICS_KERNELS_PLANE_CHECK_H
#define OVT_SRC_METRICS_KERNELS_PLANE_CHECK_H

#include <cstdint>
#include <cstring>

#include "ovt/abi/metric_v1.h"

namespace ovt::metrics {

[[nodiscard]] inline bool code_value_plane_ok(const ovt_MetricPlanes& p) noexcept {
  return p.struct_size >= sizeof(ovt_MetricPlanes) && (p.planes == 1 || p.planes == 3) &&
         p.domain == OVT_METRIC_DOMAIN_CODE_VALUE && p.sample_format == OVT_METRIC_SAMPLE_UINT &&
         (p.sample_bytes == 1 || p.sample_bytes == 2) && p.bit_depth >= 8 && p.bit_depth <= 16 &&
         (p.sample_bytes == 2 || p.bit_depth == 8);
}

[[nodiscard]] inline bool code_value_pair_ok(const ovt_MetricPlanes& r, const ovt_MetricPlanes& d) noexcept {
  if (!code_value_plane_ok(r) || !code_value_plane_ok(d) || r.planes != d.planes || r.bit_depth != d.bit_depth ||
      r.sample_bytes != d.sample_bytes) {
    return false;
  }
  for (uint8_t p = 0; p < r.planes; ++p) {
    if (r.data[p] == nullptr || d.data[p] == nullptr || r.width[p] != d.width[p] || r.height[p] != d.height[p] ||
        r.width[p] == 0 || r.height[p] == 0) {
      return false;
    }
  }
  return true;
}

// The sample at byte offset `off` of a row (1 byte, or a little-endian uint16 read without alignment).
[[nodiscard]] inline uint32_t load_sample(const uint8_t* row, size_t x, uint32_t sample_bytes) noexcept {
  if (sample_bytes == 1) {
    return row[x];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  uint16_t v = 0;
  std::memcpy(&v, row + (x * 2), sizeof v);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return v;  // x86-64 and arm64 are little-endian; the host delivers little-endian samples (ABI header)
}

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_KERNELS_PLANE_CHECK_H
