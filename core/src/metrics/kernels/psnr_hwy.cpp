// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Google Highway version of the psnr sum of squared differences (chapter 9 §9.4.1, §9.4.2; WI-3.306), compiled
// once per target and selected at run time with HWY_DYNAMIC_DISPATCH (x86-64 SSE4/AVX2/AVX-512, arm64 NEON).
//
// Samples are widened to uint32 lanes, the absolute difference is squared in uint32 (65535^2 < 2^32) and added to a
// uint32 lane accumulator. A lane accumulator can take floor((2^32 - 1) / (2^b - 1)^2) squares before it could
// overflow (66051 at 8 bits, 4112 at 10, 256 at 12, 1 at 16), so it is flushed into a uint64 total at that cadence.
// All sums are integers, hence bit-identical to the scalar reference whatever the lane count.

// clang-format off
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "metrics/kernels/psnr_hwy.cpp"
#include <hwy/foreach_target.h>  // IWYU pragma: keep
#include <hwy/highway.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "metrics/kernels/psnr_kernels.h"

HWY_BEFORE_NAMESPACE();
namespace ovt::metrics::HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

namespace {

// Largest number of squared differences one uint32 lane can absorb without wrapping.
uint32_t flush_interval(uint32_t bit_depth) {
  const uint64_t peak = (uint64_t{1} << bit_depth) - 1;
  return static_cast<uint32_t>(std::max<uint64_t>(1, 0xFFFFFFFFULL / (peak * peak)));
}

template <class D32>
uint64_t flush(D32 d32, hn::VFromD<D32> acc) {
  HWY_ALIGN std::array<uint32_t, HWY_MAX_LANES_D(D32)> lanes;
  hn::Store(acc, d32, lanes.data());
  uint64_t total = 0;
  for (size_t i = 0; i < hn::Lanes(d32); ++i) {
    total += lanes[i];
  }
  return total;
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): the kernel walks strided picture rows with vector
// loads; widths and heights are validated by the caller (psnr_metric.cpp).
template <class T>
uint64_t sse_rows(const uint8_t* ref, ptrdiff_t ref_stride, const uint8_t* dist, ptrdiff_t dist_stride, uint32_t width,
                  uint32_t height, uint32_t bit_depth) {
  const hn::ScalableTag<uint32_t> d32;
  const hn::Rebind<T, decltype(d32)> dsrc;
  const size_t n = hn::Lanes(d32);
  const uint32_t interval = flush_interval(bit_depth);
  uint64_t total = 0;
  for (uint32_t y = 0; y < height; ++y) {
    const T* r = reinterpret_cast<const T*>(ref + (static_cast<ptrdiff_t>(y) * ref_stride));
    const T* d = reinterpret_cast<const T*>(dist + (static_cast<ptrdiff_t>(y) * dist_stride));
    auto acc = hn::Zero(d32);
    uint32_t pending = 0;
    size_t x = 0;
    for (; x + n <= width; x += n) {
      const auto a = hn::PromoteTo(d32, hn::LoadU(dsrc, r + x));
      const auto b = hn::PromoteTo(d32, hn::LoadU(dsrc, d + x));
      const auto diff = hn::Sub(hn::Max(a, b), hn::Min(a, b));
      acc = hn::Add(acc, hn::Mul(diff, diff));
      if (++pending == interval) {
        total += flush(d32, acc);
        acc = hn::Zero(d32);
        pending = 0;
      }
    }
    total += flush(d32, acc);
    for (; x < width; ++x) {  // tail shorter than one vector
      const int64_t diff = static_cast<int64_t>(r[x]) - static_cast<int64_t>(d[x]);
      total += static_cast<uint64_t>(diff * diff);
    }
  }
  return total;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

}  // namespace

uint64_t PsnrSse(const uint8_t* ref, ptrdiff_t ref_stride, const uint8_t* dist, ptrdiff_t dist_stride, uint32_t width,
                 uint32_t height, uint32_t sample_bytes, uint32_t bit_depth) {
  return sample_bytes == 1 ? sse_rows<uint8_t>(ref, ref_stride, dist, dist_stride, width, height, bit_depth)
                           : sse_rows<uint16_t>(ref, ref_stride, dist, dist_stride, width, height, bit_depth);
}

}  // namespace ovt::metrics::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace ovt::metrics {

HWY_EXPORT(PsnrSse);

uint64_t psnr_sse_hwy(const uint8_t* ref, ptrdiff_t ref_stride, const uint8_t* dist, ptrdiff_t dist_stride,
                      uint32_t width, uint32_t height, uint32_t sample_bytes, uint32_t bit_depth) {
  return HWY_DYNAMIC_DISPATCH(PsnrSse)(ref, ref_stride, dist, dist_stride, width, height, sample_bytes, bit_depth);
}

}  // namespace ovt::metrics
#endif  // HWY_ONCE
