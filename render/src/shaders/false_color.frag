// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.5 false colour and §10.7.8 gamut-clip overlay (WI-4.453): the analysis views drawn from Q in place
// of the picture. A pl_shader_custom header for a fragment pass (render/src/measure/false_color.cpp); the body calls
// ovt_false_color(). The output is sRGB-encoded BT.709 (the values below are sRGB code values / 255); the surface then
// presents it as an SDR sRGB frame, so the colours do not depend on the display target.
// Bindings: ovt_q = Q (§10.7.2: .r = max(R,G,B) nits, .g = CIE Y nits, .a = gamut flags), ovt_mode = 0 for false
// colour, 1 for the gamut-clip overlay.

float ovt_pq(float nits) {  // ST 2084 inverse EOTF of nits
  float p = pow(clamp(nits / 10000.0, 0.0, 1.0), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
}

// Table 10.7-1 on max(R,G,B), lower bounds inclusive; band 1 is a grey ramp, log-interpolated from #404040 at 1 nit
// to #C0C0C0 at 100 nits.
vec3 ovt_band_color(float nits) {
  if (nits < 1.0) return vec3(31.0, 31.0, 92.0) / 255.0;  // #1F1F5C
  if (nits < 100.0) {
    float t = clamp(log(nits) / log(100.0), 0.0, 1.0);
    return vec3((64.0 + 128.0 * t) / 255.0);
  }
  if (nits < 203.0) return vec3(46.0, 204.0, 64.0) / 255.0;    // #2ECC40
  if (nits < 1000.0) return vec3(255.0, 220.0, 0.0) / 255.0;   // #FFDC00
  if (nits < 4000.0) return vec3(255.0, 133.0, 27.0) / 255.0;  // #FF851B
  return vec3(255.0, 65.0, 54.0) / 255.0;                      // #FF4136
}

// §10.7.8: magenta #FF00FF at alpha 0x99 over pixels outside BT.709, cyan #00FFFF at alpha 0x99 over pixels outside
// P3 (which wins, being the wider gamut), over a greyscale picture.
// SPEC-ASSUMPTION(#490): the grey is the PQ code of CIE Y, which keeps SDR and HDR detail visible and does not
// depend on the display target.
vec3 ovt_gamut_color(vec4 q) {
  vec3 grey = vec3(ovt_pq(max(q.g, 0.0)));
  uint flags = uint(q.a + 0.5);
  const float a = 153.0 / 255.0;  // 0x99
  if ((flags & 2u) != 0u) return mix(grey, vec3(0.0, 1.0, 1.0), a);
  if ((flags & 1u) != 0u) return mix(grey, vec3(1.0, 0.0, 1.0), a);
  return grey;
}

vec4 ovt_false_color() {
  vec4 q = texelFetch(ovt_q, ivec2(gl_FragCoord.xy), 0);
  vec3 rgb = ovt_mode == 0 ? ovt_band_color(q.r) : ovt_gamut_color(q);
  return vec4(rgb, 1.0);
}
