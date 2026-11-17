// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Selection outline, probe crosshair and selection rectangle (chapter 10 §10.5.1 "Selection / probe / rect",
// §10.5.2 rows 15, 17 and 18, §10.5.6 passes 10 and 12; WI-2.436 step 3).

#include "overlay/pass_misc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

#include "ovt/render/overlay_colors.h"

namespace ovt::render::detail {
namespace {

constexpr size_t kRowSelection = 15;
constexpr size_t kRowProbe = 17;
constexpr size_t kRowSelectionRect = 18;
constexpr int kFloatsPerVertex = 6;  // x, y (device px), r, g, b, a
constexpr int kVerticesPerRect = 6;

constexpr bool on(uint32_t layers, OverlayLayer l) {
  return (layers & static_cast<uint32_t>(l)) != 0;
}

struct Box {
  float x0, y0, x1, y1;  // NOLINT(readability-isolate-declaration)
};

void push(std::vector<MarkRect>& out, const Box& b, uint32_t rgba) {
  if (b.x1 > b.x0 && b.y1 > b.y0 && (rgba & 0xFFU) != 0) {
    out.push_back({b.x0, b.y0, b.x1, b.y1, rgba});
  }
}

// The band between `outer` and `inner` as four rectangles (top, bottom, left, right); all of `outer` when `inner` is
// empty.
void ring(std::vector<MarkRect>& out, const Box& outer, const Box& inner, uint32_t rgba) {
  if (inner.x1 <= inner.x0 || inner.y1 <= inner.y0) {
    push(out, outer, rgba);
    return;
  }
  push(out, {outer.x0, outer.y0, outer.x1, inner.y0}, rgba);
  push(out, {outer.x0, inner.y1, outer.x1, outer.y1}, rgba);
  push(out, {outer.x0, inner.y0, inner.x0, inner.y1}, rgba);
  push(out, {inner.x1, inner.y0, outer.x1, inner.y1}, rgba);
}

Box grow(const Box& b, float d) {
  return {b.x0 - d, b.y0 - d, b.x1 + d, b.y1 + d};
}

// The device box of the image rect (x0, y0)-(x1, y1), edges rounded to whole pixels.
Box device_box(const MarkView& v, float x0, float y0, float x1, float y1) {
  const auto dev = [&](float image, size_t axis) {
    return std::round(((image - v.pan.at(axis)) * v.zoom) + v.viewport_half.at(axis));
  };
  return {dev(x0, 0), dev(y0, 1), dev(x1, 0), dev(y1, 1)};
}

// Pass 10: a 2-unit row-15 band centred on the rect's edges and a 1-unit #000000 halo outside it (§10.5.2).
void selection_outline(std::vector<MarkRect>& out, const MarkView& v, const std::array<int32_t, 4>& r, float unit) {
  if (r[2] <= r[0] || r[3] <= r[1]) {
    return;
  }
  const Box edge = device_box(v, static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]),
                              static_cast<float>(r[3]));
  ring(out, grow(edge, 2.0F * unit), grow(edge, unit), kOverlayHalo.at(kRowSelection));
  ring(out, grow(edge, unit), grow(edge, -unit), kOverlayColors.at(kRowSelection));
}

// Pass 12: the crosshair through the centre of sample (x, y), across the drawn picture.
void probe_crosshair(std::vector<MarkRect>& out, const MarkView& v, int x, int y, float unit) {
  const auto start = [&](int image, size_t axis) {
    const float dev = ((static_cast<float>(image) + 0.5F - v.pan.at(axis)) * v.zoom) + v.viewport_half.at(axis);
    return std::floor(dev) - std::floor(unit * 0.5F);
  };
  const float sx = start(x, 0);
  const float sy = start(y, 1);
  const uint32_t halo = kOverlayHalo.at(kRowProbe);
  const auto& c = v.clip;
  push(out, {c[0], sy - unit, c[2], sy}, halo);
  push(out, {c[0], sy + unit, c[2], sy + (2.0F * unit)}, halo);
  push(out, {sx - unit, c[1], sx, c[3]}, halo);
  push(out, {sx + unit, c[1], sx + (2.0F * unit), c[3]}, halo);
  const uint32_t line = kOverlayColors.at(kRowProbe);
  push(out, {c[0], sy, c[2], sy + unit}, line);
  push(out, {sx, c[1], sx + unit, c[3]}, line);
}

// Pass 12: the selection rectangle, a translucent fill inside a 1-unit border, halo outside.
void selection_rect(std::vector<MarkRect>& out, const MarkView& v, const ovt::fb::Rect& r, float unit) {
  if (r.w() <= 0 || r.h() <= 0) {
    return;
  }
  const Box box = device_box(v, static_cast<float>(r.x()), static_cast<float>(r.y()),
                             static_cast<float>(int64_t{r.x()} + r.w()), static_cast<float>(int64_t{r.y()} + r.h()));
  const Box inner = grow(box, -unit);
  push(out, inner, overlay_fill_rgba(kRowSelectionRect, false));
  ring(out, grow(box, unit), box, kOverlayHalo.at(kRowSelectionRect));
  ring(out, box, inner, kOverlayColors.at(kRowSelectionRect));
}

constexpr const char* kVertexShader = R"glsl(#version 450
layout(std430, push_constant) uniform PushC {
  vec2 viewport_half;
  vec2 pad;
  vec4 clip;
} pc;
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 v_color;
void main() {
  v_color = color;
  gl_Position = vec4((pos / pc.viewport_half) - 1.0, 0.0, 1.0);
}
)glsl";

constexpr const char* kFragmentShader = R"glsl(#version 450
layout(std430, push_constant) uniform PushC {
  vec2 viewport_half;
  vec2 pad;
  vec4 clip;
} pc;
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;
void main() {
  vec2 p = gl_FragCoord.xy;
  if (p.x < pc.clip.x || p.y < pc.clip.y || p.x >= pc.clip.z || p.y >= pc.clip.w) {
    discard;
  }
  out_color = v_color;
}
)glsl";

struct MarkConstants {
  std::array<float, 2> viewport_half{};
  std::array<float, 2> pad{};
  std::array<float, 4> clip{};
};
static_assert(sizeof(MarkConstants) == 32, "must match the std430 layout of the mark shaders");

pl_pass create_mark_pass(pl_gpu gpu, pl_fmt target_format, std::string& err) {
  if (!gpu->glsl.vulkan) {
    err = "overlay passes need libplacebo's Vulkan backend";
    return nullptr;
  }
  pl_fmt pos_fmt = pl_find_vertex_fmt(gpu, PL_FMT_FLOAT, 2);
  pl_fmt color_fmt = pl_find_vertex_fmt(gpu, PL_FMT_FLOAT, 4);
  if (pos_fmt == nullptr || color_fmt == nullptr) {
    err = "no vec2 / vec4 vertex format for the selection pass";
    return nullptr;
  }
  std::array<struct pl_vertex_attrib, 2> attribs{};
  attribs[0] = {.name = "pos", .fmt = pos_fmt, .offset = 0, .location = 0};
  attribs[1] = {.name = "color", .fmt = color_fmt, .offset = 2 * sizeof(float), .location = 1};
  // Straight-alpha fragments composited "over" the premultiplied layer, as in pass_fill.cpp.
  struct pl_blend_params blend = {};
  blend.src_rgb = PL_BLEND_SRC_ALPHA;
  blend.dst_rgb = PL_BLEND_ONE_MINUS_SRC_ALPHA;
  blend.src_alpha = PL_BLEND_ONE;
  blend.dst_alpha = PL_BLEND_ONE_MINUS_SRC_ALPHA;
  struct pl_pass_params params = {};
  params.type = PL_PASS_RASTER;
  params.push_constants_size = sizeof(MarkConstants);
  params.glsl_shader = kFragmentShader;
  params.vertex_type = PL_PRIM_TRIANGLE_LIST;
  params.vertex_attribs = attribs.data();
  params.num_vertex_attribs = static_cast<int>(attribs.size());
  params.vertex_stride = kFloatsPerVertex * sizeof(float);
  params.vertex_shader = kVertexShader;
  params.target_format = target_format;
  params.blend_params = &blend;
  params.load_target = true;
  pl_pass p = pl_pass_create(gpu, &params);
  if (p == nullptr) {
    err = "pl_pass_create failed for the selection / probe pass";
  }
  return p;
}

}  // namespace

void build_marks(const OverlaySet& set, const OverlayConfig& cfg, const MarkView& view, std::vector<MarkRect>& out) {
  out.clear();
  const float unit = std::max(1.0F, std::round(view.dpr));
  if (on(cfg.layers, OverlayLayer::Selection)) {
    // SPEC-ASSUMPTION(#746): the hover outline (row 16) needs a hovered block, which OverlayConfig does not carry;
    // pass 10 draws the selection only. selected_mv outlines MotionVector.rect.
    if (cfg.selected_block < set.block_rects.size()) {
      selection_outline(out, view, set.block_rects[cfg.selected_block], unit);
    }
    if (cfg.selected_mv >= 0 && static_cast<size_t>(cfg.selected_mv) < set.mv_rects.size()) {
      selection_outline(out, view, set.mv_rects[static_cast<size_t>(cfg.selected_mv)], unit);
    }
  }
  if (on(cfg.layers, OverlayLayer::Probe) && cfg.probe_x >= 0 && cfg.probe_y >= 0) {
    probe_crosshair(out, view, cfg.probe_x, cfg.probe_y, unit);
  }
  if (on(cfg.layers, OverlayLayer::SelectionRect)) {
    selection_rect(out, view, cfg.selection_rect, unit);
  }
}

bool MarkPass::run(pl_gpu gpu, pl_tex target, const std::vector<MarkRect>& rects, const MarkView& view,
                   std::string& err) {
  if (rects.empty()) {
    return true;
  }
  if (pass_ == nullptr) {
    pass_ = create_mark_pass(gpu, target->params.format, err);
    if (pass_ == nullptr) {
      return false;
    }
  }
  vertices_.clear();
  vertices_.reserve(rects.size() * kVerticesPerRect * kFloatsPerVertex);
  for (const MarkRect& r : rects) {
    const std::array<float, 4> c = {
        static_cast<float>((r.rgba >> 24U) & 0xFFU) / 255.0F, static_cast<float>((r.rgba >> 16U) & 0xFFU) / 255.0F,
        static_cast<float>((r.rgba >> 8U) & 0xFFU) / 255.0F, static_cast<float>(r.rgba & 0xFFU) / 255.0F};
    const std::array<std::array<float, 2>, kVerticesPerRect> corners = {
        {{r.x0, r.y0}, {r.x1, r.y0}, {r.x0, r.y1}, {r.x0, r.y1}, {r.x1, r.y0}, {r.x1, r.y1}}};
    for (const auto& p : corners) {
      vertices_.insert(vertices_.end(), {p[0], p[1], c[0], c[1], c[2], c[3]});
    }
  }
  MarkConstants pc;
  pc.viewport_half = view.viewport_half;
  pc.clip = view.clip;
  struct pl_pass_run_params run = {};
  run.pass = pass_;
  run.push_constants = &pc;
  run.target = target;
  run.vertex_count = static_cast<int>(rects.size()) * kVerticesPerRect;
  run.vertex_data = vertices_.data();
  pl_pass_run(gpu, &run);
  return true;
}

void MarkPass::destroy(pl_gpu gpu) {
  pl_pass_destroy(gpu, &pass_);
}

}  // namespace ovt::render::detail
