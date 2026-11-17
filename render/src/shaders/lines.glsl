// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Grid line pass (§10.5.3, WI-2.432), compiled once with OVT_VERTEX and once with OVT_FRAGMENT after the sampler
// declarations (`ends`: RGBA32F texel per line-list vertex {x, y, colour row, 0}, luma samples; `palette`: §10.5.2).
// Vertex pulling: vertex i belongs to line i / 6, whose endpoints are texels 2 * line and 2 * line + 1; the line becomes
// a quad on whole device pixels. Junctions are covered once: horizontal bands own them (half-open at the end, closed at
// the picture's right edge), vertical bands stop between them. Slice/tile lines (opaque) are closed and dashed 8/4 by
// the length along the line from the picture origin, not gl_FragCoord, so the pattern does not jump on pan or zoom.

layout(std430, push_constant) uniform PushC {
  vec2 viewport_half;
  vec2 pan;
  vec4 clip;
  vec2 picture;
  float zoom;
  float line_scale;
  int tex_w;
  int pad;
} pc;

const int kColorGridCtu = 8;
const int kColorPartition = 12;

#ifdef OVT_VERTEX
layout(location = 0) in uint corner;
layout(location = 0) out float v_along;
layout(location = 1) flat out int v_color;

vec4 endpoint(int i) {
  return texelFetch(ends, ivec2(i % pc.tex_w, i / pc.tex_w), 0);
}

// Start of the w-px band of a line at device coordinate p, kept inside the picture [lo, hi).
float band(float p, float w, float lo, float hi) {
  float s = round(p) - floor(w * 0.5);
  return clamp(s, round(lo), max(round(hi) - w, round(lo)));
}

void main() {
  int line = gl_VertexIndex / 6;
  vec4 a = endpoint(2 * line);
  vec4 b = endpoint((2 * line) + 1);
  int color = int(a.z + 0.5);
  bool thick = color == kColorGridCtu || color == kColorPartition;
  float w = max(1.0, round((thick ? 2.0 : 1.0) * pc.line_scale));
  vec2 da = ((a.xy - pc.pan) * pc.zoom) + pc.viewport_half;
  vec2 db = ((b.xy - pc.pan) * pc.zoom) + pc.viewport_half;
  bool closed = color == kColorPartition;
  vec4 q;  // x0, y0, x1, y1 in device px
  if (a.y == b.y) {  // horizontal
    float y = band(da.y, w, pc.clip.y, pc.clip.w);
    float x0 = band(da.x, w, pc.clip.x, pc.clip.z);
    float x1 = band(db.x, w, pc.clip.x, pc.clip.z);
    if (closed || b.x >= pc.picture.x) {
      x1 += w;
    }
    q = vec4(x0, y, x1, y + w);
  } else {  // vertical
    float x = band(da.x, w, pc.clip.x, pc.clip.z);
    float y0 = band(da.y, w, pc.clip.y, pc.clip.w);
    float y1 = band(db.y, w, pc.clip.y, pc.clip.w);
    if (closed) {
      y1 += w;
    } else {
      y0 += w;
    }
    q = vec4(x, y0, x + w, y1);
  }
  int c = int(corner);
  vec2 p = vec2((c == 1 || c == 4 || c == 5) ? q.z : q.x, (c == 2 || c == 3 || c == 5) ? q.w : q.y);
  if (q.z <= q.x || q.w <= q.y) {
    p = q.xy;  // degenerate: all six vertices identical, nothing is rasterised
  }
  v_along = a.y == b.y ? p.x - pc.clip.x : p.y - pc.clip.y;
  v_color = color;
  gl_Position = vec4((p / pc.viewport_half) - 1.0, 0.0, 1.0);
}
#endif

#ifdef OVT_FRAGMENT
layout(location = 0) in float v_along;
layout(location = 1) flat in int v_color;
layout(location = 0) out vec4 out_color;

void main() {
  vec2 p = gl_FragCoord.xy;
  if (p.x < pc.clip.x || p.y < pc.clip.y || p.x >= pc.clip.z || p.y >= pc.clip.w) {
    discard;
  }
  if (v_color == kColorPartition) {
    float unit = max(1.0, round(pc.line_scale));
    if (mod(v_along, 12.0 * unit) >= 8.0 * unit) {
      discard;
    }
  }
  out_color = texelFetch(palette, ivec2(v_color, 0), 0);
}
#endif
