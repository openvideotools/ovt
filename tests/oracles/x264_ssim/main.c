// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// x264_ssim: per-frame SSIM of two Y4M files with x264's own kernel (chapter 14 §14.4.12, WI-0.565).
//
//   x264_ssim <ref>.y4m <dist>.y4m      SPEC-ASSUMPTION(#306): the WI-0.565 form, not chapter 9's <WxH> <bitdepth> form
//
// Prints one line per frame, `n:<i> Y:<ssim> U:<ssim> V:<ssim>` (i from 0, %.9f), the form that
// tests/oracles/normalize/x264_ssim.py reads. Input: 4:2:0 only, 8-bit (C420, C420jpeg, C420mpeg2, C420paldv, C420p8)
// or 10-bit (C420p10). The two files must have the same size, bit depth and frame count.
//
// The plane is offset by 2 samples and cut to (W-2)x(H-2) before the kernel runs, exactly as x264's encoder calls
// x264_pixel_ssim_wxh (chapter 9 §9.4.4 step 1); the plane value is ssim_sum / cnt. Chroma planes (an OVT addition,
// x264 prints luma only) go through the same call with their own size.
//
// Exit status: 0 done, 1 bad input (unreadable, unsupported, mismatched), 2 usage.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stddef.h>
#include <stdint.h>

// The two bit-depth builds of pixel_ssim.c (CMakeLists.txt) export their entry points under different names. Their
// pixel type differs (uint8_t / uint16_t) and so does the type of the function table, which is two function pointers
// in both; main.c holds it as opaque storage and passes the pointers through void *.
typedef struct {
  void* fn[2];
} pf_t;
float x264_pixel_ssim_wxh_8(pf_t* pf, void* p1, intptr_t s1, void* p2, intptr_t s2, int w, int h, void* buf, int* cnt);
float x264_pixel_ssim_wxh_10(pf_t* pf, void* p1, intptr_t s1, void* p2, intptr_t s2, int w, int h, void* buf, int* cnt);
void x264_ssim_functions_8(pf_t* pf);
void x264_ssim_functions_10(pf_t* pf);

typedef struct {
  FILE* f;
  const char* name;
  int w, h, depth;
  long frames_read;
} Y4m;

static int fail(const char* what, const char* name) {
  fprintf(stderr, "x264_ssim: %s: %s\n", name, what);
  return 1;
}

static int open_y4m(Y4m* y, const char* name) {
  char line[1024];
  y->f = fopen(name, "rb");
  y->name = name;
  y->depth = 8;
  y->w = y->h = 0;
  y->frames_read = 0;
  if (!y->f) return fail("cannot open", name);
  if (!fgets(line, sizeof line, y->f) || strncmp(line, "YUV4MPEG2", 9) != 0) return fail("not a Y4M file", name);
  for (char* tok = strtok(line + 9, " \n"); tok; tok = strtok(NULL, " \n")) {
    if (tok[0] == 'W')
      y->w = atoi(tok + 1);
    else if (tok[0] == 'H')
      y->h = atoi(tok + 1);
    else if (tok[0] == 'C') {
      if (!strcmp(tok + 1, "420p10"))
        y->depth = 10;
      else if (strcmp(tok + 1, "420") && strcmp(tok + 1, "420jpeg") && strcmp(tok + 1, "420mpeg2") &&
               strcmp(tok + 1, "420paldv") && strcmp(tok + 1, "420p8"))
        return fail("unsupported chroma format (4:2:0, 8 or 10 bit only)", name);
    }
  }
  if (y->w < 16 || y->h < 16 || y->w > 16384 || y->h > 16384) return fail("missing or unsupported frame size", name);
  return 0;
}

// 1 = frame read, 0 = end of file, -1 = error
static int read_frame(Y4m* y, void* planes[3], size_t bytes[3]) {
  char line[256];
  if (!fgets(line, sizeof line, y->f)) return 0;
  if (strncmp(line, "FRAME", 5) != 0) return fail("bad frame marker", y->name), -1;
  for (int i = 0; i < 3; i++)
    if (fread(planes[i], 1, bytes[i], y->f) != bytes[i]) return fail("truncated frame", y->name), -1;
  y->frames_read++;
  return 1;
}

static float plane_ssim(int depth, pf_t* pf, const void* a, const void* b, int w, int h, void* buf) {
  int cnt = 0;
  size_t bps = depth > 8 ? 2 : 1;
  // pix + 2 + 2*stride, width - 2, height - 2
  const char* pa = (const char*)a + (size_t)(2 + 2 * w) * bps;
  const char* pb = (const char*)b + (size_t)(2 + 2 * w) * bps;
  float sum = depth > 8 ? x264_pixel_ssim_wxh_10(pf, (void*)pa, w, (void*)pb, w, w - 2, h - 2, buf, &cnt)
                        : x264_pixel_ssim_wxh_8(pf, (void*)pa, w, (void*)pb, w, w - 2, h - 2, buf, &cnt);
  return cnt > 0 ? sum / (float)cnt : 1.0f;
}

int main(int argc, char** argv) {
  Y4m a, b;
  if (argc != 3) {
    fprintf(stderr, "usage: x264_ssim <ref>.y4m <dist>.y4m\n");
    return 2;
  }
  if (open_y4m(&a, argv[1]) || open_y4m(&b, argv[2])) return 1;
  if (a.w != b.w || a.h != b.h || a.depth != b.depth)
    return fail("size or bit depth differs from the reference", argv[2]);
  int cw = a.w / 2, ch = a.h / 2;
  size_t bps = a.depth > 8 ? 2 : 1;
  size_t bytes[3] = {(size_t)a.w * a.h * bps, (size_t)cw * ch * bps, (size_t)cw * ch * bps};
  int dims[3][2] = {{a.w, a.h}, {cw, ch}, {cw, ch}};
  void *ra[3], *rb[3];
  for (int i = 0; i < 3; i++) {
    ra[i] = malloc(bytes[i]);
    rb[i] = malloc(bytes[i]);
    if (!ra[i] || !rb[i]) return fail("out of memory", argv[1]);
  }
  // x264 needs 2 rows of ((width >> 2) + 3) int[4] sums
  void* buf = malloc(sizeof(int) * 4 * 2 * ((size_t)(a.w >> 2) + 3));
  pf_t pf;
  if (a.depth > 8)
    x264_ssim_functions_10(&pf);
  else
    x264_ssim_functions_8(&pf);
  if (!buf) return fail("out of memory", argv[1]);
  long n = 0;
  for (;;) {
    int ga = read_frame(&a, ra, bytes), gb = read_frame(&b, rb, bytes);
    if (ga < 0 || gb < 0) return 1;
    if (ga != gb) return fail("the two files have a different number of frames", ga ? argv[2] : argv[1]);
    if (!ga) break;
    float s[3];
    for (int i = 0; i < 3; i++) s[i] = plane_ssim(a.depth, &pf, ra[i], rb[i], dims[i][0], dims[i][1], buf);
    printf("n:%ld Y:%.9f U:%.9f V:%.9f\n", n++, (double)s[0], (double)s[1], (double)s[2]);
  }
  return 0;
}
