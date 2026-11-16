// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Compare-mode passes of chapter 10 §10.6.4 (WI-1.410), embedded into libovt-render at build time as the `header` of a
// libplacebo custom shader (pl_shader_custom). Each pass selects its part with a prelude #define (OVT_PASS_DIFF,
// OVT_PASS_PSNR, OVT_PASS_BLEND, OVT_PASS_FILL); descriptors, variables and the vertex attribute `ovt_pos` are
// #defined by libplacebo to their fresh identifiers. `ovt_pos` is the device pixel position (difference, blend, fill)
// or the plane row position (PSNR) at the fragment centre.

// Code value of a plane texel: textures are R8/R16 UNORM holding LSB-aligned codes (§10.3.1), `scale` = 2^depth - 1
// of the texture.
float ovt_code(sampler2D t, ivec2 p, float scale) {
  return floor(texelFetch(t, p, 0).r * scale + 0.5);
}

// ---- output encodings (enc: 0 sRGB code values, 1 linear with 1.0 = SDR white, 2 PQ BT.2020, 3 HLG BT.2020) ----
vec3 ovt_srgb_eotf(vec3 v) {
  return mix(v / 12.92, pow((v + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), v));
}

vec3 ovt_srgb_oetf(vec3 l) {
  l = clamp(l, 0.0, 1.0);
  return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l));
}

vec3 ovt_pq_oetf(vec3 nits) {
  vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

vec3 ovt_hlg_oetf(vec3 e) {
  e = max(e, 0.0);
  return mix(sqrt(3.0 * e), 0.17883277 * log(max(12.0 * e - 0.28466892, 1e-6)) + 0.55991073,
             step(vec3(1.0 / 12.0), e));
}

// Linear light in the target primaries (1.0 = 203 nits, libplacebo's convention) -> output code values.
vec3 ovt_encode_linear(vec3 lin, int enc) {
  if (enc == 0) {
    return ovt_srgb_oetf(lin);
  }
  if (enc == 2) {
    return ovt_pq_oetf(lin * 203.0);
  }
  if (enc == 3) {
    return ovt_hlg_oetf(lin * 0.26497);  // SDR white -> HLG 0.75 (BT.2408)
  }
  return lin;
}

// An sRGB-coded BT.709 SDR colour (the §10.6.4 difference frame, the divider white) -> output code values.
vec3 ovt_encode_srgb709(vec3 c, int enc) {
  if (enc == 0) {
    return c;  // written directly as the output code value (§10.6.4)
  }
  vec3 lin = ovt_srgb_eotf(c);
  if (enc >= 2) {
    // BT.709 -> BT.2020 (BT.2087), column-major.
    lin = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956) * lin;
  }
  return ovt_encode_linear(lin, enc);
}

#ifdef OVT_PASS_DIFF
// §10.6.4 Difference: code-value domain per plane, B already converted to A's grid and bit depth on upload.
// Variables: ovt_pan, ovt_centre (vec2), ovt_zoom, ovt_sa, ovt_sb, ovt_m, ovt_gain (float), ovt_crop (ivec4 x0 y0 x1 y1
// in luma samples), ovt_sub (ivec2), ovt_plane, ovt_signed, ovt_enc, ovt_hatch (int).
vec4 ovt_difference() {
  if (ovt_hatch != 0) {  // RND-0010: B has another size and may not be resampled
    float k = mod(floor((ovt_pos.x + ovt_pos.y) / 8.0), 2.0);
    return vec4(ovt_encode_srgb709(vec3(0.25 + 0.25 * k), ovt_enc), 1.0);
  }
  vec2 img = ovt_pan + (ovt_pos - ovt_centre) / ovt_zoom;
  ivec2 l = clamp(ivec2(floor(img)), ovt_crop.xy, ovt_crop.zw - 1);
  float d = (ovt_code(ovt_a0, l, ovt_sa) - ovt_code(ovt_b0, l, ovt_sb)) / ovt_m;
#ifdef OVT_HAS_CHROMA
  ivec2 c = clamp(l / ovt_sub, ivec2(0), textureSize(ovt_a1, 0) - 1);  // chroma sampled nearest to luma
  float du = (ovt_code(ovt_a1, c, ovt_sa) - ovt_code(ovt_b1, c, ovt_sb)) / ovt_m;
  float dv = (ovt_code(ovt_a2, c, ovt_sa) - ovt_code(ovt_b2, c, ovt_sb)) / ovt_m;
  if (ovt_plane == 1) {
    d = du;
  } else if (ovt_plane == 2) {
    d = dv;
  } else if (ovt_plane < 0) {  // max over planes (the signed view keeps the sign of the largest)
    if (abs(du) > abs(d)) {
      d = du;
    }
    if (abs(dv) > abs(d)) {
      d = dv;
    }
  }
#endif
  float g = ovt_signed != 0 ? clamp(0.5 + ovt_gain * d / 2.0, 0.0, 1.0) : clamp(ovt_gain * abs(d), 0.0, 1.0);
  vec3 rgb = vec3(g);
#ifdef OVT_FALSE_COLOR
  rgb = texelFetch(ovt_lut, ivec2(int(floor(g * 255.0 + 0.5)), 0), 0).rgb;
#endif
  return vec4(ovt_encode_srgb709(rgb, ovt_enc), 1.0);
}
#endif

#ifdef OVT_PASS_PSNR
// Sum of squared code-value differences of one plane row over [ovt_x0, ovt_x1) (the visible region, §10.6.4).
vec4 ovt_psnr_row() {
  int y = int(floor(ovt_pos.y));
  float sum = 0.0;
  for (int x = ovt_x0; x < ovt_x1; ++x) {
    float d = ovt_code(ovt_pa, ivec2(x, y), ovt_sa) - ovt_code(ovt_pb, ivec2(x, y), ovt_sb);
    sum += d * d;
  }
  return vec4(sum, 0.0, 0.0, 1.0);
}
#endif

#ifdef OVT_PASS_BLEND
// §10.6.4 Overlay: out = (1 - a) A + a B on the linear FP16 renders of A and B.
vec4 ovt_blend() {
  ivec2 p = ivec2(floor(ovt_pos));
  vec3 a = texelFetch(ovt_ia, p, 0).rgb;
  vec3 b = texelFetch(ovt_ib, p, 0).rgb;
  return vec4(ovt_encode_linear(mix(a, b, ovt_alpha), ovt_enc), 1.0);
}
#endif

#ifdef OVT_PASS_FILL
// Divider lines: SDR white in the output encoding.
vec4 ovt_fill() {
  return vec4(ovt_encode_srgb709(vec3(1.0), ovt_enc), 1.0);
}
#endif
