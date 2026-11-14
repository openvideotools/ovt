// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay pass 4, skip hatch (chapter 10 §10.5.2 row 4, §10.5.6; WI-2.431): 45° lines with a period of 4 device px
// (scaled by the device pixel ratio), one device px wide along x, colour `palette` row 4 over transparent. The pattern
// is anchored to the device pixel grid, so it neither moves nor rescales with pan and zoom. Fragment shader body: the
// prelude of pass_fill.cpp declares `pc`, `out_color`, ovt_cell(), `cells` and `palette`. GLSL 3.30 / ES 3.00
// compatible.

void main() {
  ivec2 cell;
  if (!ovt_cell(cell)) {
    discard;
  }
  if ((texelFetch(cells, cell, 0).a & 1u) == 0u) {
    discard;  // attribute A bit0 = skip
  }
  vec2 p = floor(gl_FragCoord.xy);
  if (mod(p.x + p.y, 4.0 * pc.dpr) >= pc.dpr) {
    discard;
  }
  out_color = texelFetch(palette, ivec2(4, 0), 0);
}
