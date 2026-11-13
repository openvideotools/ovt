// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The sum-of-squared-differences kernel of the psnr metric (chapter 9 §9.4.2, WI-3.306). Two implementations of one
// function: a scalar reference (psnr_scalar.cpp) and a Google Highway version with runtime dispatch (psnr_hwy.cpp).
// Both are exact integer sums, so they are bit-identical by construction (§9.4.1); the tests assert it.

#ifndef OVT_SRC_METRICS_KERNELS_PSNR_KERNELS_H
#define OVT_SRC_METRICS_KERNELS_PSNR_KERNELS_H

#include <cstddef>
#include <cstdint>

namespace ovt::metrics {

// Sum over a width x height rectangle of (ref - dist)^2. Samples are unsigned integers of `sample_bytes` (1 or 2;
// 2 = little-endian uint16 with `bit_depth` significant bits, 8..16). Strides are row pitches in bytes. The sum is
// exact: the largest possible term is 65535^2, so a uint64 accumulator holds any picture of up to 2^32 samples.
using SseFn = uint64_t (*)(const uint8_t* ref, ptrdiff_t ref_stride, const uint8_t* dist, ptrdiff_t dist_stride,
                           uint32_t width, uint32_t height, uint32_t sample_bytes, uint32_t bit_depth);

[[nodiscard]] uint64_t psnr_sse_scalar(const uint8_t* ref, ptrdiff_t ref_stride, const uint8_t* dist,
                                       ptrdiff_t dist_stride, uint32_t width, uint32_t height, uint32_t sample_bytes,
                                       uint32_t bit_depth);
// Dispatches to the best Highway target the CPU supports (HWY_DYNAMIC_DISPATCH).
[[nodiscard]] uint64_t psnr_sse_hwy(const uint8_t* ref, ptrdiff_t ref_stride, const uint8_t* dist,
                                    ptrdiff_t dist_stride, uint32_t width, uint32_t height, uint32_t sample_bytes,
                                    uint32_t bit_depth);

// Rows per band (§9.4.1 Tiling): the bands of a plane are summed in ascending order.
inline constexpr uint32_t kPsnrBandRows = 64;

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_KERNELS_PSNR_KERNELS_H
