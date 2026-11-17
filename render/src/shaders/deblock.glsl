// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay pass 7, deblocking edges (chapter 10 §10.5.1, §10.5.2 rows 13-14, §10.5.6; WI-2.436). Fragment shader body
// after the prelude of pass_fill.cpp (`pc`, `out_color`, ovt_cell()), with the samplers `edges` (RG8UI per cell:
// R = class of the cell's left edge, G = of its top edge; 0 none, 1 weak, 2 strong), `palette` (§10.5.2 colours) and
// `halo` (kOverlayHalo, one texel per row). Each edge is a band of round(dpr) device px snapped like the grid lines
// (start = round(edge) - floor(width / 2)), with the row's 1-px halo on both sides. The pass runs only when a cell is
// at least 8 device px (§10.5.1), so the nearest cell boundary is the only edge a pixel can touch.
// SPEC-ASSUMPTION(#745): §10.5.1 describes a line list; the per-cell texture draws the same bands, bounded by the
// viewport's pixels instead of a 24 MB worst-case list, with no rebuild on zoom or pan.

const int kRowWeak = 13;  // Deblock Bs = 1; row 14 = Bs 2

// Offset of device coordinate p (pixel left edge) from the start of the band of the cell boundary k along one axis.
float band_offset(float p, int k, float pan, float half_size, float w) {
  float edge = round(((4.0 * float(k)) - pan) * pc.zoom + half_size);
  return p - (edge - floor(w * 0.5));
}

// The class of the edge, or 0 at the picture boundary (not filtered).
uint edge_class(ivec2 cell, bool vertical) {
  if (cell.x < 0 || cell.y < 0 || cell.x >= pc.cells.x || cell.y >= pc.cells.y || (vertical ? cell.x : cell.y) == 0) {
    return 0u;
  }
  uvec4 e = texelFetch(edges, cell, 0);
  return vertical ? e.r : e.g;
}

void main() {
  ivec2 cell;
  if (!ovt_cell(cell)) {
    discard;
  }
  float w = max(1.0, round(pc.dpr));
  vec2 p = floor(gl_FragCoord.xy);
  vec2 image = (gl_FragCoord.xy - pc.viewport_half) / pc.zoom + pc.pan;
  ivec2 k = ivec2(round(image / 4.0));  // nearest vertical / horizontal cell boundary
  float dx = band_offset(p.x, k.x, pc.pan.x, pc.viewport_half.x, w);
  float dy = band_offset(p.y, k.y, pc.pan.y, pc.viewport_half.y, w);
  uint cv = edge_class(ivec2(k.x, cell.y), true);
  uint ch = edge_class(ivec2(cell.x, k.y), false);
  uint core = max((dx >= 0.0 && dx < w) ? cv : 0u, (dy >= 0.0 && dy < w) ? ch : 0u);
  if (core != 0u) {
    out_color = texelFetch(palette, ivec2(kRowWeak + int(core) - 1, 0), 0);
    return;
  }
  // The 1-px halo next to either side of a band; a strong edge (row 14) has none (kOverlayHalo).
  vec4 h = vec4(0.0);
  if (cv != 0u && dx >= -w && dx < 2.0 * w) {
    h = texelFetch(halo, ivec2(kRowWeak + int(cv) - 1, 0), 0);
  }
  if (h.a <= 0.0 && ch != 0u && dy >= -w && dy < 2.0 * w) {
    h = texelFetch(halo, ivec2(kRowWeak + int(ch) - 1, 0), 0);
  }
  if (h.a <= 0.0) {
    discard;
  }
  out_color = h;
}
