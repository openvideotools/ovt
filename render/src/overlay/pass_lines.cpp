// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Grid line pass with viewport and level culling (chapter 10 §10.5.1, §10.5.3, §10.5.6 passes 5-6; WI-2.432).

#include "overlay/pass_lines.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

#include "ovt/render/overlay_set.h"
#include "shaders/lines_glsl.h"

namespace ovt::render::detail {
namespace {

constexpr uint32_t kGridLayerShift = 3;  // OverlayLayer::GridTU = 1u << 3, then PU, CU, CTU
constexpr size_t kPartitionSlot = 4;
constexpr std::array<uint8_t, 4> kLevelColor = {11, 10, 9, kColorGridCtu};  // TU, PU, CU, CTU
constexpr int kCornersPerLine = 6;                                          // K of §10.5: two triangles per line
constexpr int kMaxEndpointTexW = 4096;  // endpoint texture width; rows grow with the line count

bool intersects(const GridNode& n, const ImageRect& r) {
  return static_cast<float>(n.x1) > r.x0 && static_cast<float>(n.x0)<r.x1&& static_cast<float>(n.y1)> r.y0 &&
         static_cast<float>(n.y0) < r.y1;
}

void emit(std::vector<GridVertex>& v, float x0, float y0, float x1, float y1, uint8_t color) {
  v.push_back({x0, y0, color, {}});
  v.push_back({x1, y1, color, {}});
}

// Left/top rule of §10.5.3: the top and left edges, plus the right / bottom edge on the picture boundary.
void emit_block(const GridNode& n, float pw, float ph, std::vector<GridVertex>& v) {
  const uint8_t color = kLevelColor.at(static_cast<size_t>(n.level));
  const auto x0 = static_cast<float>(n.x0);
  const auto y0 = static_cast<float>(n.y0);
  const auto x1 = static_cast<float>(n.x1);
  const auto y1 = static_cast<float>(n.y1);
  emit(v, x0, y0, x1, y0, color);  // top
  emit(v, x0, y0, x0, y1, color);  // left
  if (x1 >= pw) {
    emit(v, x1, y0, x1, y1, color);  // right picture boundary
  }
  if (y1 >= ph) {
    emit(v, x0, y1, x1, y1, color);  // bottom picture boundary
  }
}

}  // namespace

ImageRect visible_image_rect(int w, int h, double pan_x, double pan_y, double zoom) {
  const double z = zoom > 0.0 ? zoom : 1.0;
  const double hw = static_cast<double>(w) / (2.0 * z);
  const double hh = static_cast<double>(h) / (2.0 * z);
  return {static_cast<float>(pan_x - hw), static_cast<float>(pan_y - hh), static_cast<float>(pan_x + hw),
          static_cast<float>(pan_y + hh)};
}

ImageRect expand_margin(const ImageRect& r) {
  const float mx = (r.x1 - r.x0) * kGridMargin;
  const float my = (r.y1 - r.y0) * kGridMargin;
  return {r.x0 - mx, r.y0 - my, r.x1 + mx, r.y1 + my};
}

uint32_t grid_levels_drawn(const OverlaySet& set, uint32_t layers, double zoom) {
  uint32_t drawn = 0;
  for (uint32_t level = 0; level < 4; ++level) {
    if ((layers & (1U << (kGridLayerShift + level))) == 0) {
      continue;
    }
    const int32_t side = set.grid_min_side.at(level);
    if (side == std::numeric_limits<int32_t>::max()) {
      continue;  // the frame has no block of this level
    }
    // R-GRID-1: never draw sub-6-px grids.
    if (static_cast<double>(side) * zoom >= static_cast<double>(kGridMinPx)) {
      drawn |= 1U << level;
    }
  }
  return drawn;
}

void build_grid_lines(const OverlaySet& set, uint32_t levels, bool with_partitions, const ImageRect& area,
                      GridLines& out) {
  out.vertices.clear();
  out.level_vertices = {};
  out.levels = levels;
  out.area = area;
  std::array<std::vector<GridVertex>, 4> per_level;
  if (levels != 0 && !set.grid_nodes.empty()) {
    const auto pw = static_cast<float>(set.picture_w);
    const auto ph = static_cast<float>(set.picture_h);
    std::vector<uint32_t> stack = {0};
    while (!stack.empty()) {
      const GridNode& n = set.grid_nodes[stack.back()];
      stack.pop_back();
      if (n.level != GridLevel::None && (levels & (1U << static_cast<uint32_t>(n.level))) != 0) {
        emit_block(n, pw, ph, per_level.at(static_cast<size_t>(n.level)));
      }
      // Children in reverse so that they are visited (and emitted) in tree order: the list is deterministic.
      for (uint32_t i = n.child_count; i > 0; --i) {
        const uint32_t c = n.first_child + i - 1;
        if (c < set.grid_nodes.size() && intersects(set.grid_nodes[c], area)) {
          stack.push_back(c);
        }
      }
    }
  }
  for (size_t level = 0; level < per_level.size(); ++level) {  // §10.5.6: TU -> PU -> CU -> CTU
    out.level_vertices.at(level) = static_cast<uint32_t>(per_level.at(level).size());
    out.vertices.insert(out.vertices.end(), per_level.at(level).begin(), per_level.at(level).end());
  }
  if (with_partitions) {
    const size_t before = out.vertices.size();
    for (const PartitionLine& l : set.partition_lines) {
      emit(out.vertices, static_cast<float>(l.x0), static_cast<float>(l.y0), static_cast<float>(l.x1),
           static_cast<float>(l.y1), kColorPartition);
    }
    out.level_vertices.at(kPartitionSlot) = static_cast<uint32_t>(out.vertices.size() - before);
  }
}

bool GridLineCache::update(const OverlaySet& set, uint32_t layers, double zoom, const ImageRect& visible) {
  const uint32_t levels = grid_levels_drawn(set, layers, zoom);
  const bool partitions = (layers & static_cast<uint32_t>(OverlayLayer::GridPartition)) != 0;
  const bool inside = visible.x0 >= lines_.area.x0 && visible.y0 >= lines_.area.y0 && visible.x1 <= lines_.area.x1 &&
                      visible.y1 <= lines_.area.y1;
  // Without a drawn grid level the area does not matter: panning never rebuilds the partition-only list.
  if (valid_ && set_ == &set && layers_ == layers && lines_.levels == levels && (levels == 0 || inside)) {
    return false;
  }
  build_grid_lines(set, levels, partitions, expand_margin(visible), lines_);
  set_ = &set;
  layers_ = layers;
  valid_ = true;
  ++builds_;
  return true;
}

namespace {

std::string shader_source(const char* stage, bool fragment) {
  std::string src = "#version 450\n#define ";
  src += stage;
  src += "\n";
  src +=
      fragment ? "layout(binding = 1) uniform sampler2D palette;\n" : "layout(binding = 0) uniform sampler2D ends;\n";
  src += shaders::kLines;
  return src;
}

pl_pass create_line_pass(pl_gpu gpu, pl_fmt target_format, pl_fmt corner_fmt, std::string& err) {
  if (!gpu->glsl.vulkan) {
    err = "overlay passes need libplacebo's Vulkan backend";
    return nullptr;
  }
  if (gpu->limits.max_pushc_size < sizeof(LinePassConstants)) {
    err = "the line pass needs " + std::to_string(sizeof(LinePassConstants)) + " bytes of push constants";
    return nullptr;
  }
  const std::string vertex = shader_source("OVT_VERTEX", false);
  const std::string fragment = shader_source("OVT_FRAGMENT", true);
  std::array<struct pl_desc, 2> descs = {
      pl_desc{.name = "ends", .type = PL_DESC_SAMPLED_TEX, .binding = 0, .access = PL_DESC_ACCESS_READONLY},
      pl_desc{.name = "palette", .type = PL_DESC_SAMPLED_TEX, .binding = 1, .access = PL_DESC_ACCESS_READONLY}};
  struct pl_vertex_attrib attrib = {};
  attrib.name = "corner";
  attrib.fmt = corner_fmt;
  attrib.offset = 0;
  attrib.location = 0;
  // Straight-alpha fragments "over" a premultiplied layer, as the cell passes (§10.5, #467).
  struct pl_blend_params blend = {};
  blend.src_rgb = PL_BLEND_SRC_ALPHA;
  blend.dst_rgb = PL_BLEND_ONE_MINUS_SRC_ALPHA;
  blend.src_alpha = PL_BLEND_ONE;
  blend.dst_alpha = PL_BLEND_ONE_MINUS_SRC_ALPHA;
  struct pl_pass_params params = {};
  params.type = PL_PASS_RASTER;
  params.descriptors = descs.data();
  params.num_descriptors = static_cast<int>(descs.size());
  params.push_constants_size = sizeof(LinePassConstants);
  params.glsl_shader = fragment.c_str();
  params.vertex_type = PL_PRIM_TRIANGLE_LIST;
  params.vertex_attribs = &attrib;
  params.num_vertex_attribs = 1;
  params.vertex_stride = corner_fmt->texel_size;
  params.vertex_shader = vertex.c_str();
  params.target_format = target_format;
  params.blend_params = &blend;
  params.load_target = true;
  pl_pass p = pl_pass_create(gpu, &params);
  if (p == nullptr) {
    err = "pl_pass_create failed for the grid line pass";
  }
  return p;
}

// The per-vertex corner attribute: an 8-bit unsigned integer where the GPU takes it as a vertex format with the
// required stride alignment, else 32 bits.
pl_fmt find_corner_fmt(pl_gpu gpu) {
  for (const char* name : {"r8u", "r16u", "r32u"}) {
    pl_fmt f = pl_find_named_fmt(gpu, name);
    if (f != nullptr && (f->caps & PL_FMT_CAP_VERTEX) != 0 &&
        (gpu->limits.align_vertex_stride == 0 || f->texel_size % gpu->limits.align_vertex_stride == 0)) {
      return f;
    }
  }
  return nullptr;
}

}  // namespace

bool LinePass::ensure_corners(pl_gpu gpu, int vertex_count, std::string& err) {
  if (corner_fmt_ == nullptr) {
    corner_fmt_ = find_corner_fmt(gpu);
    if (corner_fmt_ == nullptr) {
      err = "no integer vertex format for the grid line pass";
      return false;
    }
  }
  if (corners_ != nullptr && corner_vertices_ >= vertex_count) {
    return true;
  }
  // Grown on demand to the next power of two; the content is the static pattern 0..5 per line.
  int n = std::max(corner_vertices_, kCornersPerLine * 1024);
  while (n < vertex_count) {
    n *= 2;
  }
  const size_t ts = corner_fmt_->texel_size;
  std::vector<uint8_t> data(static_cast<size_t>(n) * ts, 0);
  for (int i = 0; i < n; ++i) {
    data[static_cast<size_t>(i) * ts] = static_cast<uint8_t>(i % kCornersPerLine);  // little-endian low byte
  }
  pl_buf_destroy(gpu, &corners_);
  struct pl_buf_params bp = {};
  bp.size = data.size();
  bp.drawable = true;
  bp.initial_data = data.data();
  corners_ = pl_buf_create(gpu, &bp);
  if (corners_ == nullptr) {
    corner_vertices_ = 0;
    err = "pl_buf_create failed for the grid line corner buffer";
    return false;
  }
  corner_vertices_ = n;
  return true;
}

bool LinePass::upload(pl_gpu gpu, const GridLines& lines, std::string& err) {
  lines_ = 0;
  const size_t count = lines.vertices.size();
  if (count == 0) {
    return true;
  }
  const auto max_dim = static_cast<size_t>(gpu->limits.max_tex_2d_dim);
  const size_t w = std::min<size_t>(static_cast<size_t>(kMaxEndpointTexW), max_dim);
  const size_t h = (count + w - 1) / w;
  if (h > max_dim) {
    err = "grid line list of " + std::to_string(count) + " vertices exceeds the endpoint texture";
    return false;
  }
  pl_fmt fmt = pl_find_named_fmt(gpu, "rgba32f");
  if (fmt == nullptr || (fmt->caps & PL_FMT_CAP_SAMPLEABLE) == 0) {
    err = "overlay texture format rgba32f is not sampleable";
    return false;
  }
  struct pl_tex_params tp = {};
  tp.w = static_cast<int>(w);
  tp.h = static_cast<int>(h);
  tp.format = fmt;
  tp.sampleable = true;
  tp.host_writable = true;
  if (ends_ == nullptr || ends_->params.w != tp.w || ends_->params.h < tp.h) {
    if (!pl_tex_recreate(gpu, &ends_, &tp)) {
      err = "pl_tex_recreate failed for the grid line endpoints";
      return false;
    }
  }
  std::vector<float> texels(w * h * 4, 0.0F);
  for (size_t i = 0; i < count; ++i) {
    texels[(i * 4)] = lines.vertices[i].x;
    texels[(i * 4) + 1] = lines.vertices[i].y;
    texels[(i * 4) + 2] = static_cast<float>(lines.vertices[i].color_idx);
  }
  struct pl_tex_transfer_params xfer = {};
  xfer.tex = ends_;
  xfer.rc = {0, 0, 0, static_cast<int>(w), static_cast<int>(h), 1};
  xfer.row_pitch = w * 4 * sizeof(float);
  xfer.ptr = texels.data();
  if (!pl_tex_upload(gpu, &xfer)) {
    err = "pl_tex_upload failed for the grid line endpoints";
    return false;
  }
  const int vertices = static_cast<int>(count / 2) * kCornersPerLine;
  if (!ensure_corners(gpu, vertices, err)) {
    return false;
  }
  lines_ = static_cast<int>(count / 2);
  tex_w_ = static_cast<int>(w);
  return true;
}

bool LinePass::run(pl_gpu gpu, pl_tex target, pl_tex palette, LinePassConstants constants, std::string& err) {
  if (lines_ == 0) {
    return true;
  }
  if (pass_ == nullptr) {
    pass_ = create_line_pass(gpu, target->params.format, corner_fmt_, err);
    if (pass_ == nullptr) {
      return false;
    }
  }
  std::array<struct pl_desc_binding, 2> bindings{};
  bindings[0].object = ends_;
  bindings[1].object = palette;
  for (auto& b : bindings) {
    b.address_mode = PL_TEX_ADDRESS_CLAMP;
    b.sample_mode = PL_TEX_SAMPLE_NEAREST;  // texelFetch only
  }
  constants.tex_w = tex_w_;
  struct pl_pass_run_params run = {};
  run.pass = pass_;
  run.desc_bindings = bindings.data();
  run.push_constants = &constants;
  run.target = target;
  run.vertex_count = lines_ * kCornersPerLine;
  run.vertex_buf = corners_;
  pl_pass_run(gpu, &run);
  return true;
}

void LinePass::destroy(pl_gpu gpu) {
  pl_pass_destroy(gpu, &pass_);
  pl_tex_destroy(gpu, &ends_);
  pl_buf_destroy(gpu, &corners_);
  corner_vertices_ = 0;
  lines_ = 0;
}

}  // namespace ovt::render::detail
