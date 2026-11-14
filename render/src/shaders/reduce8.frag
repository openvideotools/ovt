// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.3 (WI-4.452): one 8x8 -> 1 step of the MaxCLL / FALL / min reduction chain.
// A pl_shader_custom header for a fragment pass (render/src/measure/reduce.cpp); the body calls ovt_reduce8().
// Bindings: ovt_src = the previous level, ovt_first = 1 when ovt_src is Q (reads Q.r = max(R,G,B) nits), 0 when it is
// an earlier reduction level (reads .r = max, .g = sum, .b = min). Output texel = (max, sum, min, 0) of its 8x8 block;
// texels past the right or bottom edge of ovt_src are skipped, so every input texel is counted exactly once. The
// result does not depend on which output texel a block lands in, so a flipped gl_FragCoord origin changes nothing.

vec4 ovt_reduce8() {
  ivec2 size = textureSize(ovt_src, 0);
  ivec2 base = ivec2(gl_FragCoord.xy) * 8;
  float mx = -1.0e30;
  float mn = 1.0e30;
  float sm = 0.0;
  for (int dy = 0; dy < 8; ++dy) {
    float row = 0.0;
    for (int dx = 0; dx < 8; ++dx) {
      ivec2 p = base + ivec2(dx, dy);
      if (p.x < size.x && p.y < size.y) {
        vec4 v = texelFetch(ovt_src, p, 0);
        if (ovt_first != 0) {
          mx = max(mx, v.r);
          mn = min(mn, v.r);
          row += v.r;
        } else {
          mx = max(mx, v.r);
          mn = min(mn, v.b);
          row += v.g;
        }
      }
    }
    sm += row;
  }
  return vec4(mx, sm, mn, 0.0);
}
