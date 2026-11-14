// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.6-§10.7.7 (WI-4.454): accumulation of the luminance waveform, the vectorscope and the CIE xy scope
// from the measurement texture M. A pl_shader_custom header for a compute pass with 16x16 work groups
// (render/src/measure/scopes.cpp); the body calls ovt_scopes(). render/src/measure/scopes.h has the CPU reference of
// every mapping.
// SPEC-ASSUMPTION(#473): integer atomics in a compute pass replace the point-accumulation raster pass of §10.7.3, as for
// the histogram. Counts are exact uint32 and independent of execution order, so the result is the same on every run.
//
// Bindings: ovt_m = M (linear light, content primaries, 1.0 = 203 nits), ovt_step = subsampling factor s, ovt_grid =
// sampled grid size (ceil(w / s), ceil(h / s)), ovt_crop_w = M's width, ovt_xyz_x/y/z = rows of the content RGB -> XYZ
// matrix, ovt_b2020_r/g/b = rows of the content RGB -> BT.2020 RGB matrix, ovt_wf_axis (0 log nits, 1 PQ code),
// ovt_wf_parade (0 luminance, 1 RGB parade), ovt_vs_gain, ovt_counts = uint storage buffer, cleared before the pass:
// [0, 3 x 1024 x 512) waveform planes (Y or R, G, B), then 512 x 512 vectorscope, then 512 x 512 CIE xy, each row-major
// with texel row 0 at the top.

const uint OVT_WF_W = 1024u;
const uint OVT_WF_H = 512u;
const uint OVT_WF_PLANE = OVT_WF_W * OVT_WF_H;
const uint OVT_VS_BASE = 3u * OVT_WF_PLANE;
const uint OVT_CIE_BASE = OVT_VS_BASE + 512u * 512u;

float ovt_scope_pq(float y) {  // ST 2084 Eq. 5.1 (inverse EOTF), y = nits / 10000
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float p = pow(clamp(y, 0.0, 1.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// §10.7.6 row, 0 = darkest; the texel row is 511 - row (brightest at the top).
uint ovt_wf_row(float nits) {
  float f;
  if (ovt_wf_axis == 0) {
    f = 511.0 * (log2(clamp(nits, 1.0e-4, 1.0e4)) * 0.30102999566398120 + 4.0) / 8.0;
  } else {
    f = 511.0 * ovt_scope_pq(nits / 10000.0);
  }
  return uint(clamp(floor(f), 0.0, 511.0));
}

void ovt_wf_add(uint plane, uint col, float nits) {
  uint ty = 511u - ovt_wf_row(nits);
  atomicAdd(ovt_counts[plane * OVT_WF_PLANE + ty * OVT_WF_W + col], 1u);
}

// A point of a 512 x 512 plot: within one texel of the edge it is clamped onto the edge, further out it is dropped.
void ovt_plot(uint base, vec2 uv) {
  if (uv.x >= -1.0 && uv.y >= -1.0 && uv.x <= 513.0 && uv.y <= 513.0) {
    ivec2 p = clamp(ivec2(floor(uv)), ivec2(0), ivec2(511));
    atomicAdd(ovt_counts[base + uint(p.y) * 512u + uint(p.x)], 1u);
  }
}

void ovt_scopes() {
  ivec2 g = ivec2(gl_GlobalInvocationID.xy);
  if (g.x >= ovt_grid.x || g.y >= ovt_grid.y) {
    return;
  }
  ivec2 pos = g * ovt_step;
  vec3 rgb = texelFetch(ovt_m, pos, 0).rgb * 203.0;  // nits, content primaries

  // §10.7.6 waveform: column from the sample's x in the crop.
  uint col = (uint(pos.x) * OVT_WF_W) / uint(ovt_crop_w);
  vec3 xyz = vec3(dot(ovt_xyz_x, rgb), dot(ovt_xyz_y, rgb), dot(ovt_xyz_z, rgb));
  if (ovt_wf_parade == 0) {
    ovt_wf_add(0u, col, xyz.y);
  } else {
    ovt_wf_add(0u, col, rgb.r);
    ovt_wf_add(1u, col, rgb.g);
    ovt_wf_add(2u, col, rgb.b);
  }

  // §10.7.7 vectorscope: BT.2020 NCL Cb, Cr of the PQ signal.
  vec3 c = max(vec3(dot(ovt_b2020_r, rgb), dot(ovt_b2020_g, rgb), dot(ovt_b2020_b, rgb)), vec3(0.0));
  vec3 e = vec3(ovt_scope_pq(c.r / 10000.0), ovt_scope_pq(c.g / 10000.0), ovt_scope_pq(c.b / 10000.0));
  float yp = dot(e, vec3(0.2627, 0.6780, 0.0593));
  float cb = (e.b - yp) / 1.8814;
  float cr = (e.r - yp) / 1.4746;
  ovt_plot(OVT_VS_BASE, vec2(256.0 + cb * 512.0 * ovt_vs_gain, 256.0 - cr * 512.0 * ovt_vs_gain));

  // §10.7.7 CIE xy; black has no chromaticity and is skipped.
  float sum = xyz.x + xyz.y + xyz.z;
  if (sum > 1.0e-6) {
    vec2 xy = xyz.xy / sum;
    ovt_plot(OVT_CIE_BASE, vec2(xy.x / 0.8 * 512.0, (1.0 - xy.y / 0.9) * 512.0));
  }
}
