// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.3 (WI-4.452): the 1024-bin PQ histogram of max(R,G,B) over the sampled pixels, and (WI-4.453) the
// per-band counts of Table 10.7-1 (§10.7.5) and the out-of-gamut counts (§10.7.8) over the same samples.
// A pl_shader_custom header for a compute pass with 16x16 work groups (render/src/measure/histogram.cpp); the body
// calls ovt_accumulate().
// SPEC-ASSUMPTION(#473): integer atomics in a compute pass replace the point-accumulation raster pass of §10.7.3.
// Counts are exact uint32 and independent of execution order, so the result is the same on every run.
// Bindings: ovt_q = Q (.r = max(R,G,B) nits, .b = its PQ code, .a = gamut flags), ovt_step = the subsampling factor s,
// ovt_grid = the sampled grid size (ceil(w / s), ceil(h / s)), ovt_hist = uint[1024] and ovt_counts = uint[8] (bands
// 0-5, outside BT.709, outside P3) in one storage buffer, cleared before the pass. Sample (i, j) reads Q at
// (i * s, j * s): the top-left pixel of each s x s block.

shared uint ovt_lh[1024];
shared uint ovt_lc[8];

// Table 10.7-1, lower bounds inclusive. SPEC-ASSUMPTION(#490): counted on max(R,G,B) itself rather than read off
// the PQ histogram, whose 1/1024 bins straddle the band edges.
uint ovt_band(float nits) {
  if (nits < 1.0) return 0u;
  if (nits < 100.0) return 1u;
  if (nits < 203.0) return 2u;
  if (nits < 1000.0) return 3u;
  if (nits < 4000.0) return 4u;
  return 5u;
}

void ovt_accumulate() {
  uint li = gl_LocalInvocationIndex;
  for (uint i = li; i < 1024u; i += 256u) {
    ovt_lh[i] = 0u;
  }
  if (li < 8u) {
    ovt_lc[li] = 0u;
  }
  memoryBarrierShared();
  barrier();
  ivec2 g = ivec2(gl_GlobalInvocationID.xy);
  if (g.x < ovt_grid.x && g.y < ovt_grid.y) {
    vec4 q = texelFetch(ovt_q, g * ovt_step, 0);
    uint bin = uint(clamp(floor(q.b * 1024.0), 0.0, 1023.0));
    atomicAdd(ovt_lh[bin], 1u);
    atomicAdd(ovt_lc[ovt_band(q.r)], 1u);
    uint flags = uint(q.a + 0.5);
    if ((flags & 1u) != 0u) {
      atomicAdd(ovt_lc[6], 1u);
    }
    if ((flags & 2u) != 0u) {
      atomicAdd(ovt_lc[7], 1u);
    }
  }
  memoryBarrierShared();
  barrier();
  for (uint i = li; i < 1024u; i += 256u) {
    if (ovt_lh[i] != 0u) {
      atomicAdd(ovt_hist[i], ovt_lh[i]);
    }
  }
  if (li < 8u && ovt_lc[li] != 0u) {
    atomicAdd(ovt_counts[li], ovt_lc[li]);
  }
}
