// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.3 (WI-4.452): the 1024-bin PQ histogram of max(R,G,B) over the sampled pixels.
// A pl_shader_custom header for a compute pass with 16x16 work groups (render/src/measure/histogram.cpp); the body
// calls ovt_accumulate().
// SPEC-ASSUMPTION(#473): integer atomics in a compute pass replace the point-accumulation raster pass of §10.7.3.
// Counts are exact uint32 and independent of execution order, so the result is the same on every run.
// Bindings: ovt_q = Q (.b = PQ code of max(R,G,B)), ovt_step = the subsampling factor s, ovt_grid = the sampled grid
// size (ceil(w / s), ceil(h / s)), ovt_hist = uint[1024] storage buffer, cleared before the pass. Sample (i, j) reads
// Q at (i * s, j * s): the top-left pixel of each s x s block.

shared uint ovt_lh[1024];

void ovt_accumulate() {
  uint li = gl_LocalInvocationIndex;
  for (uint i = li; i < 1024u; i += 256u) {
    ovt_lh[i] = 0u;
  }
  memoryBarrierShared();
  barrier();
  ivec2 g = ivec2(gl_GlobalInvocationID.xy);
  if (g.x < ovt_grid.x && g.y < ovt_grid.y) {
    float pq = texelFetch(ovt_q, g * ovt_step, 0).b;
    uint bin = uint(clamp(floor(pq * 1024.0), 0.0, 1023.0));
    atomicAdd(ovt_lh[bin], 1u);
  }
  memoryBarrierShared();
  barrier();
  for (uint i = li; i < 1024u; i += 256u) {
    if (ovt_lh[i] != 0u) {
      atomicAdd(ovt_hist[i], ovt_lh[i]);
    }
  }
}
