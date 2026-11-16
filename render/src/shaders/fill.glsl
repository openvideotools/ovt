// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay pass 3, prediction-kind and reference-index fills (chapter 10 §10.5.1, §10.5.2, §10.5.6; WI-2.431).
// Fragment shader body: the prelude of pass_fill.cpp declares `pc`, `out_color`, ovt_cell(), the cell attribute
// texture `cells` (RGBA8UI: R PredKind, G ref0 + 1, B ref1 + 1, A flags) and `palette` (the §10.5.2 colours with
// their fill alphas, one texel per row). GLSL 3.30 / ES 3.00 compatible.

const uint kLayerPredFill = 2u;      // OverlayLayer::PredFill
const uint kLayerRefIdxFill = 8192u; // OverlayLayer::RefIdxFill

vec4 pal(int index) {
  return texelFetch(palette, ivec2(index, 0), 0);
}

// ref + 1 in 1..254 is a reference; 0 = none, 255 = the plugin gave no ref vector.
bool has_ref(uint v) {
  return v >= 1u && v <= 254u;
}

vec4 pred_color(uvec4 a) {
  switch (a.r) {
    case 1u:  // Intra
      return pal(0);
    case 2u:  // Inter
    case 3u:  // Skip: inter-predicted; the skip hatch (pass 4) marks it on top
      if (has_ref(a.g) && has_ref(a.b)) {
        return pal(3);
      }
      return has_ref(a.b) && !has_ref(a.g) ? pal(2) : pal(1);
    case 4u:  // IntraBC
      return pal(5);
    case 5u:  // PCM
      return pal(6);
    case 6u:  // Palette
      return pal(7);
    default:  // None, or no pred vector (0xFF)
      return vec4(0.0);
  }
}

// Ref slot 0..7 of list 0, else of list 1 (§10.5.2 rows 22-29).
vec4 ref_color(uvec4 a) {
  if (a.g >= 1u && a.g <= 8u) {
    return pal(22 + int(a.g) - 1);
  }
  if (a.b >= 1u && a.b <= 8u) {
    return pal(22 + int(a.b) - 1);
  }
  return vec4(0.0);
}

void main() {
  ivec2 cell;
  if (!ovt_cell(cell)) {
    discard;
  }
  uvec4 a = texelFetch(cells, cell, 0);
  vec4 c = vec4(0.0);
  if ((pc.layers & kLayerPredFill) != 0u) {
    c = pred_color(a);
  }
  if ((pc.layers & kLayerRefIdxFill) != 0u) {
    // The reference-index tint goes over the prediction tint (straight-alpha "over").
    vec4 r = ref_color(a);
    float out_a = r.a + c.a * (1.0 - r.a);
    c = out_a > 0.0 ? vec4((r.rgb * r.a + c.rgb * c.a * (1.0 - r.a)) / out_a, out_a) : vec4(0.0);
  }
  if (c.a <= 0.0) {
    discard;
  }
  out_color = c;
}
