// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.2 (WI-4.452): per-pixel quantities of the measurement texture M, written to Q (RGBA16F).
// A pl_shader_custom header for a fragment pass (render/src/measure/reduce.cpp); the body calls ovt_measure_prep().
// Bindings (pl_shader_custom #defines): ovt_m = M (linear light, content primaries, 1.0 = 203 nits),
// ovt_luma = the Y row of the content RGB -> XYZ matrix (§10.6.3).
//
// Q = (max(R,G,B) nits, CIE Y nits, PQ(max(R,G,B)), out-of-gamut flags). The flags of §10.7.8 belong to WI-4.453 and
// are 0 here. max(R,G,B) is clamped at 0: a negative component is outside the content gamut, not negative light.

float ovt_pq_oetf(float y) {  // ST 2084 Eq. 5.1 (inverse EOTF), y = nits / 10000
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float p = pow(clamp(y, 0.0, 1.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

vec4 ovt_measure_prep() {
  vec3 rgb = texelFetch(ovt_m, ivec2(gl_FragCoord.xy), 0).rgb * 203.0;
  float maxrgb = max(max(rgb.r, max(rgb.g, rgb.b)), 0.0);
  float y = dot(rgb, ovt_luma);
  return vec4(maxrgb, y, ovt_pq_oetf(maxrgb / 10000.0), 0.0);
}
