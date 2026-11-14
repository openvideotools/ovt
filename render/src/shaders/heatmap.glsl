// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay pass 2, heatmap (chapter 10 §10.5.5, §10.5.6; WI-2.431). Fragment shader body: the prelude of
// pass_fill.cpp declares the push constants `pc`, `out_color`, ovt_cell() and the samplers `heat` (R16_UNORM value),
// `heat_valid` (R8, 0 = not applicable) and `lut` (256x1 RGBA8). GLSL 3.30 / ES 3.00 compatible.
//
// The LUT is read with texelFetch at the nearest of its 256 entries: no filtering, so every GPU picks the same entry.

void main() {
  ivec2 cell;
  if (!ovt_cell(cell)) {
    discard;
  }
  if (texelFetch(heat_valid, cell, 0).r < 0.5) {
    discard;  // "not applicable" cells (-1 / 0xFF) get alpha 0
  }
  float v = clamp(texelFetch(heat, cell, 0).r, 0.0, 1.0);
  int idx = int(floor(v * 255.0 + 0.5));
  out_color = vec4(texelFetch(lut, ivec2(idx, 0), 0).rgb, pc.alpha);
}
