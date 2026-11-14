// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Scalar reference of the psnr sum of squared differences (chapter 9 §9.4.1, §9.4.2; WI-3.306).

#include <cstdint>
#include <cstring>

#include "metrics/kernels/psnr_kernels.h"

namespace ovt::metrics {

namespace {

inline uint32_t load_u16(const uint8_t* p) {
  uint16_t v = 0;
  std::memcpy(&v, p, sizeof v);  // little-endian samples (ovt_MetricPlanes); the host is little-endian
  return v;
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-implicit-widening-of-multiplication-result):
// the reference walks strided picture rows; the caller validates the geometry.
uint64_t psnr_sse_scalar(const uint8_t* ref, ptrdiff_t ref_stride, const uint8_t* dist, ptrdiff_t dist_stride,
                         uint32_t width, uint32_t height, uint32_t sample_bytes, uint32_t /*bit_depth*/) {
  uint64_t sse = 0;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* r = ref + (static_cast<ptrdiff_t>(y) * ref_stride);
    const uint8_t* d = dist + (static_cast<ptrdiff_t>(y) * dist_stride);
    if (sample_bytes == 1) {
      for (uint32_t x = 0; x < width; ++x) {
        const int32_t diff = static_cast<int32_t>(r[x]) - static_cast<int32_t>(d[x]);
        sse += static_cast<uint64_t>(static_cast<int64_t>(diff) * diff);
      }
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        const int64_t diff =
            static_cast<int64_t>(load_u16(r + (size_t{2} * x))) - static_cast<int64_t>(load_u16(d + (size_t{2} * x)));
        sse += static_cast<uint64_t>(diff * diff);
      }
    }
  }
  return sse;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-implicit-widening-of-multiplication-result)

}  // namespace ovt::metrics
