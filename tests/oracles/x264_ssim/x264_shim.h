// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The x264 definitions that pixel_ssim.c (x264 common/pixel.c, r3191) needs: the pixel type of the compile-time
 * BIT_DEPTH, PIXEL_MAX, XCHG, X264_MIN, and the two members of x264_pixel_function_t the SSIM code calls. x264's
 * own definitions are in common/common.h, common/base.h and common/pixel.h.
 *
 * Build one object per bit depth: -DBIT_DEPTH=8 or -DBIT_DEPTH=10, with the entry point renamed
 * (-Dx264_pixel_ssim_wxh=x264_pixel_ssim_wxh_8 ...), as CMakeLists.txt does.
 */
#ifndef X264_SSIM_SHIM_H
#define X264_SSIM_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifndef BIT_DEPTH
#error "define BIT_DEPTH (8 or 10)"
#endif

#if BIT_DEPTH > 8
typedef uint16_t pixel;
#else
typedef uint8_t pixel;
#endif

#define PIXEL_MAX ((1 << BIT_DEPTH) - 1)
#define XCHG(type, a, b) \
  do {                   \
    type t_ = (a);       \
    (a) = (b);           \
    (b) = t_;            \
  } while (0)
#define X264_MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct x264_pixel_function_t {
  void (*ssim_4x4x2_core)(const pixel* pix1, intptr_t stride1, const pixel* pix2, intptr_t stride2, int sums[2][4]);
  float (*ssim_end4)(int sum0[5][4], int sum1[5][4], int width);
} x264_pixel_function_t;

float x264_pixel_ssim_wxh(x264_pixel_function_t* pf, pixel* pix1, intptr_t stride1, pixel* pix2, intptr_t stride2,
                          int width, int height, void* buf, int* cnt);

#endif
