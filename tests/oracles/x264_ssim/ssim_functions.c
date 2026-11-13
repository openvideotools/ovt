// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fills x264_pixel_function_t with the C SSIM functions of pixel_ssim.c (x264 does this in x264_pixel_init with
// pixf->ssim_4x4x2_core = ssim_4x4x2_core; pixf->ssim_end4 = ssim_end4; the assembly versions are not used here).
// The functions are static in x264, so this file includes pixel_ssim.c.

#include "pixel_ssim.c"

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)

void CAT(x264_ssim_functions_, SSIM_SUFFIX)(x264_pixel_function_t* pf) {
  pf->ssim_4x4x2_core = ssim_4x4x2_core;
  pf->ssim_end4 = ssim_end4;
}
