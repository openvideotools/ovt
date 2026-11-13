// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Google Highway versions of the ssim float32 kernels (chapter 9 §9.4.3; WI-3.307), compiled once per target and
// selected at run time with HWY_DYNAMIC_DISPATCH (x86-64 SSE4/AVX2/AVX-512, arm64 NEON). Every vector loop does the
// same float32 operations in the same order as ssim_scalar.cpp (multiply then add, never fused; this file is built
// with -ffp-contract=off) and hands the tail shorter than one vector to the scalar reference, so the two versions
// are bit-identical.

// clang-format off
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "metrics/kernels/ssim_hwy.cpp"
#include <hwy/foreach_target.h>  // IWYU pragma: keep
#include <hwy/highway.h>
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "metrics/kernels/ssim_kernels.h"

HWY_BEFORE_NAMESPACE();
namespace ovt::metrics::HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): the kernels walk strided rows with vector loads; the
// caller (ssim_metric.cpp) validates the geometry and sizes the buffers (n + 10 floats per filtered row).

// Loads one vector of T samples from a byte address of any alignment (a 16-bit row can sit at an odd address): the
// bytes are loaded as uint8 and reinterpreted with BitCast, never through a misaligned const uint16_t* (as psnr, #411).
template <class T, class D>
auto load_samples(D dsrc, const uint8_t* bytes) {
  if constexpr (sizeof(T) == 1) {
    return hn::LoadU(dsrc, bytes);
  } else {
    const hn::Repartition<uint8_t, D> d8;
    return hn::BitCast(dsrc, hn::LoadU(d8, bytes));
  }
}

template <class T>
void ToFloatRows(const uint8_t* src, ptrdiff_t src_stride, uint32_t width, uint32_t height, float scale, float* dst,
                 size_t dst_stride) {
  const hn::ScalableTag<float> df;
  const hn::RebindToSigned<decltype(df)> di;
  const hn::Rebind<T, decltype(df)> dsrc;
  const size_t n = hn::Lanes(df);
  const auto vscale = hn::Set(df, scale);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* s = src + (static_cast<ptrdiff_t>(y) * src_stride);  // bytes: a 16-bit row need not be 2-aligned
    float* d = dst + (static_cast<size_t>(y) * dst_stride);
    size_t x = 0;
    for (; x + n <= width; x += n) {
      const auto v = hn::ConvertTo(df, hn::PromoteTo(di, load_samples<T>(dsrc, s + (x * sizeof(T)))));
      hn::StoreU(hn::Mul(v, vscale), df, d + x);
    }
    for (; x < width; ++x) {
      T v = 0;
      std::memcpy(&v, s + (x * sizeof(T)), sizeof v);
      d[x] = static_cast<float>(v) * scale;
    }
  }
}

void ToFloat(const uint8_t* src, ptrdiff_t src_stride, uint32_t width, uint32_t height, uint32_t sample_bytes,
             float scale, float* dst, size_t dst_stride) {
  if (sample_bytes == 1) {
    ToFloatRows<uint8_t>(src, src_stride, width, height, scale, dst, dst_stride);
  } else {
    ToFloatRows<uint16_t>(src, src_stride, width, height, scale, dst, dst_stride);
  }
}

void Products(const float* a, const float* b, float* aa, float* bb, float* ab, size_t count) {
  const hn::ScalableTag<float> df;
  const size_t n = hn::Lanes(df);
  size_t i = 0;
  for (; i + n <= count; i += n) {
    const auto va = hn::LoadU(df, a + i);
    const auto vb = hn::LoadU(df, b + i);
    hn::StoreU(hn::Mul(va, va), df, aa + i);
    hn::StoreU(hn::Mul(vb, vb), df, bb + i);
    hn::StoreU(hn::Mul(va, vb), df, ab + i);
  }
  for (; i < count; ++i) {
    aa[i] = a[i] * a[i];
    bb[i] = b[i] * b[i];
    ab[i] = a[i] * b[i];
  }
}

void HFilter(const float* in, float* out, size_t count, const float* taps) {
  const hn::ScalableTag<float> df;
  const size_t n = hn::Lanes(df);
  size_t x = 0;
  for (; x + n <= count; x += n) {
    auto sum = hn::Mul(hn::LoadU(df, in + x), hn::Set(df, taps[0]));
    for (uint32_t k = 1; k < kSsimTaps; ++k) {
      sum = hn::Add(sum, hn::Mul(hn::LoadU(df, in + x + k), hn::Set(df, taps[k])));
    }
    hn::StoreU(sum, df, out + x);
  }
  if (x < count) {
    ssim_hfilter_scalar(in + x, out + x, count - x, taps);
  }
}

void VWindow(const float* const* mom, size_t stride, size_t count, const float* taps, SsimConstants k, float* out) {
  const hn::ScalableTag<float> df;
  const size_t n = hn::Lanes(df);
  const auto c1 = hn::Set(df, k.c1);
  const auto c2 = hn::Set(df, k.c2);
  const auto c3 = hn::Set(df, k.c2 / 2.0F);
  const auto two = hn::Set(df, 2.0F);
  const auto zero = hn::Zero(df);
  size_t x = 0;
  for (; x + n <= count; x += n) {
    // Not an array of vectors: std::array<Vec128, N> fails to compile on AppleClang in a multi-target TU (bf16
    // feature).
    const auto filter = [&](const float* base) {
      auto sum = hn::Mul(hn::LoadU(df, base + x), hn::Set(df, taps[0]));
      for (uint32_t t = 1; t < kSsimTaps; ++t) {
        sum = hn::Add(sum, hn::Mul(hn::LoadU(df, base + x + (t * stride)), hn::Set(df, taps[t])));
      }
      return sum;
    };
    const auto m0 = filter(mom[0]);
    const auto m1 = filter(mom[1]);
    const auto m2 = filter(mom[2]);
    const auto m3 = filter(mom[3]);
    const auto m4 = filter(mom[4]);
    const auto mu_x = m0;
    const auto mu_y = m1;
    const auto var_x = hn::Max(hn::Sub(m2, hn::Mul(mu_x, mu_x)), zero);
    const auto var_y = hn::Max(hn::Sub(m3, hn::Mul(mu_y, mu_y)), zero);
    const auto cov = hn::Sub(m4, hn::Mul(mu_x, mu_y));
    const auto sigma = hn::Sqrt(hn::Mul(var_x, var_y));
    const auto l = hn::Div(hn::Add(hn::Mul(hn::Mul(two, mu_x), mu_y), c1),
                           hn::Add(hn::Add(hn::Mul(mu_x, mu_x), hn::Mul(mu_y, mu_y)), c1));
    const auto c = hn::Div(hn::Add(hn::Mul(two, sigma), c2), hn::Add(hn::Add(var_x, var_y), c2));
    const auto clamp = hn::And(hn::Lt(cov, zero), hn::Le(sigma, zero));
    const auto cov_c = hn::IfThenElse(clamp, zero, cov);
    const auto s = hn::Div(hn::Add(cov_c, c3), hn::Add(sigma, c3));
    hn::StoreU(hn::Mul(hn::Mul(l, c), s), df, out + x);
  }
  if (x < count) {
    const std::array<const float*, 5> shifted = {mom[0] + x, mom[1] + x, mom[2] + x, mom[3] + x, mom[4] + x};
    ssim_vwindow_scalar(shifted.data(), stride, count - x, taps, k, out + x);
  }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

}  // namespace ovt::metrics::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace ovt::metrics {

HWY_EXPORT(ToFloat);
HWY_EXPORT(Products);
HWY_EXPORT(HFilter);
HWY_EXPORT(VWindow);

void ssim_to_float_hwy(const uint8_t* src, ptrdiff_t src_stride, uint32_t width, uint32_t height, uint32_t sample_bytes,
                       float scale, float* dst, size_t dst_stride) {
  HWY_DYNAMIC_DISPATCH(ToFloat)(src, src_stride, width, height, sample_bytes, scale, dst, dst_stride);
}

void ssim_products_hwy(const float* a, const float* b, float* aa, float* bb, float* ab, size_t n) {
  HWY_DYNAMIC_DISPATCH(Products)(a, b, aa, bb, ab, n);
}

void ssim_hfilter_hwy(const float* in, float* out, size_t n, const float* taps) {
  HWY_DYNAMIC_DISPATCH(HFilter)(in, out, n, taps);
}

void ssim_vwindow_hwy(const float* const* mom, size_t stride, size_t n, const float* taps, SsimConstants k,
                      float* out) {
  HWY_DYNAMIC_DISPATCH(VWindow)(mom, stride, n, taps, k, out);
}

}  // namespace ovt::metrics
#endif  // HWY_ONCE
