// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Scalar reference of the ssim float32 kernels (chapter 9 §9.4.3; WI-3.307). Built with -ffp-contract=off so that the
// result equals the Highway version bit for bit.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "metrics/kernels/ssim_kernels.h"

namespace ovt::metrics {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): the reference walks strided picture rows and row
// bands; the caller (ssim_metric.cpp) validates the geometry.
void ssim_to_float_scalar(const uint8_t* src, ptrdiff_t src_stride, uint32_t width, uint32_t height,
                          uint32_t sample_bytes, float scale, float* dst, size_t dst_stride) {
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* s = src + (static_cast<ptrdiff_t>(y) * src_stride);
    float* d = dst + (static_cast<size_t>(y) * dst_stride);
    if (sample_bytes == 1) {
      for (uint32_t x = 0; x < width; ++x) {
        d[x] = static_cast<float>(s[x]) * scale;
      }
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        uint16_t v = 0;
        std::memcpy(&v, s + (size_t{2} * x), sizeof v);  // little-endian samples; the host is little-endian
        d[x] = static_cast<float>(v) * scale;
      }
    }
  }
}

void ssim_products_scalar(const float* a, const float* b, float* aa, float* bb, float* ab, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    aa[i] = a[i] * a[i];
    bb[i] = b[i] * b[i];
    ab[i] = a[i] * b[i];
  }
}

void ssim_hfilter_scalar(const float* in, float* out, size_t n, const float* taps) {
  for (size_t x = 0; x < n; ++x) {
    float sum = in[x] * taps[0];
    for (uint32_t k = 1; k < kSsimTaps; ++k) {
      sum = sum + (in[x + k] * taps[k]);
    }
    out[x] = sum;
  }
}

void ssim_vwindow_scalar(const float* const* mom, size_t stride, size_t n, const float* taps, SsimConstants k,
                         float* out) {
  const float c3 = k.c2 / 2.0F;
  for (size_t x = 0; x < n; ++x) {
    std::array<float, 5> m{};
    for (size_t j = 0; j < 5; ++j) {
      float sum = mom[j][x] * taps[0];
      for (uint32_t t = 1; t < kSsimTaps; ++t) {
        sum = sum + (mom[j][x + (t * stride)] * taps[t]);
      }
      m[j] = sum;
    }
    const float mu_x = m[0];
    const float mu_y = m[1];
    const float var_x = std::fmax(m[2] - (mu_x * mu_x), 0.0F);
    const float var_y = std::fmax(m[3] - (mu_y * mu_y), 0.0F);
    const float cov = m[4] - (mu_x * mu_y);
    const float sigma = std::sqrt(var_x * var_y);
    const float l = ((2.0F * mu_x * mu_y) + k.c1) / ((mu_x * mu_x) + (mu_y * mu_y) + k.c1);
    const float c = ((2.0F * sigma) + k.c2) / (var_x + var_y + k.c2);
    // A flat window with a slightly negative covariance (rounding) must score s = 1 (libvmaf's corner case fix).
    const float cov_c = (cov < 0.0F && sigma <= 0.0F) ? 0.0F : cov;
    const float s = (cov_c + c3) / (sigma + c3);
    out[x] = l * c * s;
  }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

}  // namespace ovt::metrics
