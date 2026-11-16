// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.6-§10.7.7 (WI-4.454): one scope's counts -> its RGBA16F texture. A pl_shader_custom header for a
// fragment pass (render/src/measure/scopes.cpp); the body calls ovt_scope_resolve().
// SPEC-ASSUMPTION(#512): texel = (log2(1 + count) per channel, 1); a single-channel scope repeats its value in R, G, B.
// Bindings: ovt_counts = uint storage buffer (read only), ovt_base = offset of the scope's first plane, ovt_width =
// texture width, ovt_planes = 1 or 3, ovt_stride = distance between planes. Texel (x, y) reads count
// [base + y x width + x]: gl_FragCoord is in texel units of the target, row 0 first, on every backend.

vec4 ovt_scope_resolve() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  uint i = uint(ovt_base) + uint(p.y) * uint(ovt_width) + uint(p.x);
  float r = log2(1.0 + float(ovt_counts[i]));
  if (ovt_planes == 1) {
    return vec4(r, r, r, 1.0);
  }
  float g = log2(1.0 + float(ovt_counts[i + uint(ovt_stride)]));
  float b = log2(1.0 + float(ovt_counts[i + 2u * uint(ovt_stride)]));
  return vec4(r, g, b, 1.0);
}
