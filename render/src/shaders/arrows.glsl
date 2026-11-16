// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay passes 8 (intra direction strokes) and 9 (MV arrows) of chapter 10 §10.5.6, drawn by vertex pulling
// (§10.5, §10.5.4; WI-2.434). One draw covers a range of instances; every instance has K = 9 vertices: corners 0-5
// are the shaft quad (two triangles) or the 2x2 px dot, 6-8 the head triangle. The vertex buffer holds only the
// integer corner (r8u, or r16u/r32u where the device aligns vertex strides); the instance data is fetched from two
// textures: `geom` RGBA32F {ox, oy, vx, vy} (luma samples) and `attr` RGBA32UI {color_idx | flags << 8, id, 0, 0}.
// `palette` holds the §10.5.2 colours (arrows are opaque).
//
// The prelude of pass_arrows.cpp declares `pc` (viewport_half, pan, clip, zoom, dpr, min_px, mode, first, tex_w), the
// samplers and, for the fragment stage, `out_color`; it defines OVT_VERTEX for the vertex stage. Vulkan GLSL
// (gl_VertexIndex); a GLES backend would use gl_VertexID through the same macro (§10.5).

const uint kInstBi = 1u;
const uint kInstDashed = 2u;
const uint kInstGlobal = 4u;
const uint kInstIntraStroke = 32u;
const uint kModeBiDraw = 1u;  // the bi slot of pass 9: cyan, solid
const uint kModeSkipBi = 2u;  // list draws while MvBi is on: bi instances are left to the bi slot

#ifdef OVT_VERTEX

layout(location = 0) in uint corner;
layout(location = 0) flat out vec4 v_color;
layout(location = 1) out float v_dist;  // device px along the shaft from the origin
layout(location = 2) flat out uint v_dashed;

// Outside the clip volume; every vertex of a culled instance (and of an unused triangle) is this same point, so the
// triangle is degenerate with three identical vertices and no tiler rasterises a sliver (WI-2.434 pitfall).
const vec4 kHidden = vec4(-4.0, -4.0, 0.0, 1.0);

vec2 to_device(vec2 image) {
  return (image - pc.pan) * pc.zoom + pc.viewport_half;
}

vec4 to_clip(vec2 device) {
  return vec4(device / pc.viewport_half - 1.0, 0.0, 1.0);
}

void hide() {
  gl_Position = kHidden;
  v_color = vec4(0.0);
  v_dist = 0.0;
  v_dashed = 0u;
}

void main() {
  int inst = pc.first + gl_VertexIndex / 9;
  ivec2 t = ivec2(inst % pc.tex_w, inst / pc.tex_w);
  vec4 g = texelFetch(geom, t, 0);
  uvec4 a = texelFetch(attr, t, 0);
  uint flags = a.x >> 8;
  bool bi_draw = (pc.mode & kModeBiDraw) != 0u;
  bool stroke = (flags & kInstIntraStroke) != 0u;
  if ((pc.mode & kModeSkipBi) != 0u && (flags & kInstBi) != 0u) {
    hide();
    return;
  }

  float s = pc.dpr;
  vec2 o = to_device(g.xy);
  vec2 e = to_device(g.xy + g.zw);
  // Culling: the instance's device-px box (head and dot included) misses the drawn picture.
  float m = 6.0 * s + 2.0;
  vec2 lo = min(o, e) - m;
  vec2 hi = max(o, e) + m;
  if (hi.x < pc.clip.x || hi.y < pc.clip.y || lo.x > pc.clip.z || lo.y > pc.clip.w) {
    hide();
    return;
  }

  // §10.5.4 colour rules: list 0 -> 1, list 1 -> 2, bi with MvBi -> 3, global/warped -> 21 (the builder already chose
  // 1, 2, 20 or 21 per instance).
  uint idx = a.x & 0xFFu;
  if (bi_draw && (flags & kInstGlobal) == 0u) {
    idx = 3u;
  }
  vec4 c = vec4(texelFetch(palette, ivec2(int(idx), 0), 0).rgb, 1.0);
  uint k = corner;
  float len = length(e - o);
  vec2 p;
  float dist = 0.0;
  if (len < pc.min_px * s) {
    // Too short on screen: a 2x2 px dot (x dpr) at the origin, snapped to the pixel grid. Strokes have no dot.
    if (stroke || k >= 6u) {
      hide();
      return;
    }
    vec2 ctr = floor(o + 0.5);
    vec2 q[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0),
                        vec2(1.0, 1.0));
    p = ctr + q[k] * s;
  } else {
    // Endpoints snapped to pixel centres so axis-aligned 1-px shafts are crisp.
    vec2 so = floor(o) + 0.5;
    vec2 se = floor(e) + 0.5;
    vec2 d = (e - o) / len;
    vec2 n = vec2(-d.y, d.x);
    float head = stroke ? 0.0 : min(6.0 * s, len);
    float shaft = stroke ? len : max(len - 4.0 * s, 0.0);
    if (k < 6u) {
      if (shaft <= 0.0) {
        hide();
        return;
      }
      // (0,-) (L,-) (0,+) | (0,+) (L,-) (L,+); width 1 px x dpr.
      float along = (k == 1u || k == 4u || k == 5u) ? shaft : 0.0;
      float across = (k == 2u || k == 3u || k == 5u) ? 0.5 * s : -0.5 * s;
      p = so + d * along + n * across;
      dist = along;
    } else {
      if (head <= 0.0) {
        hide();
        return;
      }
      // Isosceles head: 6 px base, 6 px long (x dpr), tip at the vector's end.
      vec2 base = se - d * head;
      p = k == 6u ? se : (k == 7u ? base + n * 3.0 * s : base - n * 3.0 * s);
    }
  }
  gl_Position = to_clip(p);
  v_color = c;
  v_dist = dist;
  // L1 shafts are dashed (§10.5.4 list coding); L0, bi (in the bi slot) and strokes are solid.
  v_dashed = (!bi_draw && !stroke && k < 6u && (flags & kInstDashed) != 0u) ? 1u : 0u;
}

#else

layout(location = 0) flat in vec4 v_color;
layout(location = 1) in float v_dist;
layout(location = 2) flat in uint v_dashed;

void main() {
  // 4 px on / 2 px off in device pixels (x dpr).
  if (v_dashed != 0u && mod(v_dist, 6.0 * pc.dpr) >= 4.0 * pc.dpr) {
    discard;
  }
  out_color = v_color;
}

#endif
