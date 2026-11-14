// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The float32 building blocks of the Gaussian ssim metric (chapter 9 §9.4.3, WI-3.307). Each function exists twice:
// a scalar reference (ssim_scalar.cpp) and a Google Highway version with runtime dispatch (ssim_hwy.cpp). Both
// perform the same float32 operations in the same order (a multiply, then an add, one tap after the other; no fused
// multiply-add, the two files are built with -ffp-contract=off), so their results are bit-identical whatever the
// vector width; the tests assert it.

#ifndef OVT_SRC_METRICS_KERNELS_SSIM_KERNELS_H
#define OVT_SRC_METRICS_KERNELS_SSIM_KERNELS_H

#include <cstddef>
#include <cstdint>

namespace ovt::metrics {

// The window is 11 x 11, separable (§9.4.3).
inline constexpr uint32_t kSsimTaps = 11;
inline constexpr uint32_t kSsimMargin = kSsimTaps - 1;  // output size = input size - 10 ("valid")
// Output rows per band: the five moment planes of a band (rows + 10 input rows) stay in cache.
inline constexpr uint32_t kSsimBandRows = 32;

// Constants of one window formula evaluation (float32, as libvmaf's _iqa_ssim computes them).
struct SsimConstants {
  float c1;
  float c2;
};

// dst[i] = float(src[i]) * scale for i < width*height rows; samples are `sample_bytes` (1 or 2, little-endian
// uint16) wide, strides in bytes. dst has row pitch `dst_stride` floats.
using ToFloatFn = void (*)(const uint8_t* src, ptrdiff_t src_stride, uint32_t width, uint32_t height,
                           uint32_t sample_bytes, float scale, float* dst, size_t dst_stride);

// aa[i] = a[i]*a[i], bb[i] = b[i]*b[i], ab[i] = a[i]*b[i] for i < n.
using ProductsFn = void (*)(const float* a, const float* b, float* aa, float* bb, float* ab, size_t n);

// out[x] = sum_{k=0..10} taps[k] * in[x + k] for x < n, accumulated k = 0 first, float32.
using HFilterFn = void (*)(const float* in, float* out, size_t n, const float* taps);

// One output row. `mom` is an array of 5 pointers; `mom[m]` points at the first of 11 consecutive rows (row pitch
// `stride` floats) of horizontally filtered moment m, in the order {x, y, x^2, y^2, x*y}. The vertical 11-tap filter
// gives mu_x, mu_y, E[x^2], E[y^2], E[xy]; the variances are E[x^2] - mu_x^2 (E[y^2] - mu_y^2) clamped to >= 0 and
// sigma_xy = E[xy] - mu_x mu_y. out[x] = l*c*s per window position (float32; libvmaf's formulation, §9.4.3).
using VWindowFn = void (*)(const float* const* mom, size_t stride, size_t n, const float* taps, SsimConstants k,
                           float* out);

void ssim_to_float_scalar(const uint8_t* src, ptrdiff_t src_stride, uint32_t width, uint32_t height,
                          uint32_t sample_bytes, float scale, float* dst, size_t dst_stride);
void ssim_products_scalar(const float* a, const float* b, float* aa, float* bb, float* ab, size_t n);
void ssim_hfilter_scalar(const float* in, float* out, size_t n, const float* taps);
void ssim_vwindow_scalar(const float* const* mom, size_t stride, size_t n, const float* taps, SsimConstants k,
                         float* out);

// Dispatch to the best Highway target the CPU supports (HWY_DYNAMIC_DISPATCH).
void ssim_to_float_hwy(const uint8_t* src, ptrdiff_t src_stride, uint32_t width, uint32_t height, uint32_t sample_bytes,
                       float scale, float* dst, size_t dst_stride);
void ssim_products_hwy(const float* a, const float* b, float* aa, float* bb, float* ab, size_t n);
void ssim_hfilter_hwy(const float* in, float* out, size_t n, const float* taps);
void ssim_vwindow_hwy(const float* const* mom, size_t stride, size_t n, const float* taps, SsimConstants k, float* out);

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_KERNELS_SSIM_KERNELS_H
