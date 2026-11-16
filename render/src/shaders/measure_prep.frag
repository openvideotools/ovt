// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.2 (WI-4.452) and §10.7.8 (WI-4.453): per-pixel quantities of the measurement texture M, written to
// Q (RGBA16F). A pl_shader_custom header for a fragment pass (render/src/measure/reduce.cpp); the body calls
// ovt_measure_prep().
// Bindings (pl_shader_custom #defines): ovt_m = M (linear light, content primaries, 1.0 = 203 nits),
// ovt_luma = the Y row of the content RGB -> XYZ matrix (§10.6.3), ovt_to709 / ovt_top3 = content RGB -> BT.709 /
// Display P3 linear RGB (via XYZ), ovt_peak = the container peak in M units (gamut_peak() in false_color.cpp).
//
// Q = (max(R,G,B) nits, CIE Y nits, PQ(max(R,G,B)), out-of-gamut flags). max(R,G,B) is clamped at 0: a negative
// component is outside the content gamut, not negative light. Flags (§10.7.8): bit0 = outside BT.709, bit1 = outside
// P3, stored as the float 0..3 (exact in half float).

float ovt_pq_oetf(float y) {  // ST 2084 Eq. 5.1 (inverse EOTF), y = nits / 10000
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float p = pow(clamp(y, 0.0, 1.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// §10.7.8 with the tolerance of tests/corpora/patterns/p3_saturation.expected.json (gamut_tolerance_linear = 0.002):
// outside when a component is negative by more than 0.2 % of the largest component (absorbing the rounding of the
// half-float M and of the 3x3 conversion), or when a component exceeds the container peak by more than 0.2 % (a colour
// brighter than the gamut's own primary can reproduce, e.g. Display P3 blue = 1.098 x BT.709 blue).
// SPEC-ASSUMPTION(#490): the tolerance and the peak; §10.7.8 leaves "maxrgb_709_rel" undefined.
float ovt_outside(vec3 c) {
  float hi = max(c.r, max(c.g, c.b));
  float tol = 0.002 * max(hi, 0.001);
  bool negative = min(c.r, min(c.g, c.b)) < -tol;
  return (negative || hi > ovt_peak * 1.002) ? 1.0 : 0.0;
}

vec4 ovt_measure_prep() {
  vec3 m = texelFetch(ovt_m, ivec2(gl_FragCoord.xy), 0).rgb;
  vec3 rgb = m * 203.0;
  float maxrgb = max(max(rgb.r, max(rgb.g, rgb.b)), 0.0);
  float y = dot(rgb, ovt_luma);
  float flags = ovt_outside(ovt_to709 * m) + 2.0 * ovt_outside(ovt_top3 * m);
  return vec4(maxrgb, y, ovt_pq_oetf(maxrgb / 10000.0), flags);
}
