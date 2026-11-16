// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay passes 8 and 9 of chapter 10 §10.5.6 (WI-2.434): intra direction strokes and MV arrows by vertex pulling
// (§10.5: the vertex buffer holds only an integer corner, the instance data lives in an RGBA32F + RGBA32UI texture pair
// read with texelFetch; K = 9 vertices per instance, §10.5.4 shaft quad + head triangle).

#include "overlay/pass_arrows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

#include "ovt/render/overlay_set.h"
#include "shaders/arrows_glsl.h"

namespace ovt::render::detail {
namespace {

// §10.5.1: K = 9 vertices per arrow as a triangle list, 6 for the shaft quad + 3 for the head (issue #645).
constexpr uint32_t kVerticesPerInstance = 9;  // gl_VertexIndex / 9 -> instance, % 9 -> corner
constexpr uint32_t kModeBiDraw = 1;
constexpr uint32_t kModeSkipBi = 2;

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

// Push constants, samplers and outputs shared by both stages (the std430 offsets are those of ArrowPassConstants).
constexpr const char* kPrelude = R"glsl(
layout(binding = 0) uniform sampler2D geom;
layout(binding = 1) uniform usampler2D attr;
layout(binding = 2) uniform sampler2D palette;
layout(std430, push_constant) uniform PushC {
  vec2 viewport_half;
  vec2 pan;
  vec4 clip;
  float zoom;
  float dpr;
  float min_px;
  uint mode;
  int first;
  int tex_w;
} pc;
)glsl";

std::string vertex_source() {
  return std::string("#version 450\n#define OVT_VERTEX 1\n") + kPrelude + shaders::kArrows;
}

std::string fragment_source() {
  return std::string("#version 450\n") + kPrelude + "layout(location = 0) out vec4 out_color;\n" + shaders::kArrows;
}

pl_tex make_instance_texture(pl_gpu gpu, const char* fmt_name, int w, int h, const void* data, std::string& err) {
  pl_fmt fmt = pl_find_named_fmt(gpu, fmt_name);
  if (fmt == nullptr || (fmt->caps & PL_FMT_CAP_SAMPLEABLE) == 0) {
    err = std::string("arrow instance format ") + fmt_name + " is not sampleable";
    return nullptr;
  }
  struct pl_tex_params params = {};
  params.w = w;
  params.h = h;
  params.format = fmt;
  params.sampleable = true;
  params.host_writable = true;
  pl_tex tex = pl_tex_create(gpu, &params);
  if (tex == nullptr) {
    err = std::string("pl_tex_create failed for an arrow ") + fmt_name + " texture";
    return nullptr;
  }
  struct pl_tex_transfer_params tp = {};
  tp.tex = tex;
  tp.row_pitch = static_cast<size_t>(w) * 16;  // both formats are 16 bytes per texel
  tp.ptr = const_cast<void*>(data);            // NOLINT(cppcoreguidelines-pro-type-const-cast): upload only reads it
  if (!pl_tex_upload(gpu, &tp)) {
    pl_tex_destroy(gpu, &tex);
    err = std::string("pl_tex_upload failed for an arrow ") + fmt_name + " texture";
    return nullptr;
  }
  return tex;
}

// The visible image rect of the view (x0, y0, x1, y1), luma samples.
std::array<float, 4> view_rect(const ArrowPassConstants& pc) {
  const float hw = pc.viewport_half[0] / pc.zoom;
  const float hh = pc.viewport_half[1] / pc.zoom;
  return {pc.pan[0] - hw, pc.pan[1] - hh, pc.pan[0] + hw, pc.pan[1] + hh};
}

// The narrowest unsigned vertex format whose texel size meets the device's vertex stride alignment: r8u on most
// devices; MoltenVK reports minVertexInputBindingStrideAlignment = 4, so r32u there. The corner lives in the low byte.
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

bool ArrowPass::set(pl_gpu gpu, const ArrowInstances* full, uint64_t culled_threshold, std::string& err) {
  drop_textures(gpu);
  full_ = full;
  culled_ = false;
  if (full == nullptr || full->size() == 0) {
    full_ = nullptr;
    return true;
  }
  if (full->size() > culled_threshold) {
    culled_ = true;  // R-8K-1: draw() uploads the instances around the viewport
    return true;
  }
  return upload(gpu, *full, err);
}

void ArrowPass::drop_textures(pl_gpu gpu) {
  pl_tex_destroy(gpu, &geom_);
  pl_tex_destroy(gpu, &attr_);
  resident_seg_ = ArrowSegments{};
  built_rect_ = {};
}

bool ArrowPass::upload(pl_gpu gpu, const ArrowInstances& inst, std::string& err) {
  pl_tex_destroy(gpu, &geom_);
  pl_tex_destroy(gpu, &attr_);
  resident_seg_ = ArrowSegments{};
  const size_t n = inst.size();
  if (n == 0) {
    return true;
  }
  const int w = std::min(kInstanceTexWidth, static_cast<int>(gpu->limits.max_tex_2d_dim));
  const size_t rows = (n + static_cast<size_t>(w) - 1) / static_cast<size_t>(w);
  if (rows > size_t{gpu->limits.max_tex_2d_dim}) {
    err = std::to_string(n) + " arrow instances exceed the instance texture size";
    return false;
  }
  // The last row is padded; padding texels are never fetched (draws stay below seg.end).
  const size_t texels = rows * static_cast<size_t>(w);
  std::vector<float> geom(texels * 4, 0.0F);
  std::vector<uint32_t> attr(texels * 4, 0U);
  std::copy(inst.geom.begin(), inst.geom.end(), geom.begin());
  std::copy(inst.attr.begin(), inst.attr.end(), attr.begin());
  geom_ = make_instance_texture(gpu, "rgba32f", w, static_cast<int>(rows), geom.data(), err);
  attr_ =
      geom_ != nullptr ? make_instance_texture(gpu, "rgba32u", w, static_cast<int>(rows), attr.data(), err) : nullptr;
  if (attr_ == nullptr) {
    pl_tex_destroy(gpu, &geom_);
    return false;
  }
  resident_seg_ = inst.seg;
  return true;
}

size_t ArrowPass::texture_bytes() const {
  size_t bytes = 0;
  for (pl_tex t : {geom_, attr_}) {
    if (t != nullptr) {
      bytes += static_cast<size_t>(t->params.w) * static_cast<size_t>(t->params.h) * 16;
    }
  }
  return bytes;
}

bool ArrowPass::ensure_view(pl_gpu gpu, const ArrowPassConstants& pc, std::string& err) {
  if (!culled_ || full_ == nullptr) {
    return true;
  }
  const std::array<float, 4> v = view_rect(pc);
  const bool inside = built_rect_[2] > built_rect_[0] && v[0] >= built_rect_[0] && v[1] >= built_rect_[1] &&
                      v[2] <= built_rect_[2] && v[3] <= built_rect_[3];
  if (inside) {
    return true;
  }
  // §10.5.8 R-8K-1: the viewport expanded by 25 % on each side; rebuilt when the viewport leaves it.
  const float mx = (v[2] - v[0]) * 0.25F;
  const float my = (v[3] - v[1]) * 0.25F;
  const std::array<float, 4> rect = {v[0] - mx, v[1] - my, v[2] + mx, v[3] + my};
  ArrowInstances subset;
  cull_instances(*full_, rect, subset);
  if (!upload(gpu, subset, err)) {
    return false;
  }
  built_rect_ = rect;
  return true;
}

bool ArrowPass::ensure_pass(pl_gpu gpu, pl_fmt target_format, std::string& err) {
  if (pass_ != nullptr) {
    return true;
  }
  if (!gpu->glsl.vulkan) {
    err = "the arrow pass needs libplacebo's Vulkan backend";
    return false;
  }
  if (gpu->limits.max_pushc_size < sizeof(ArrowPassConstants)) {
    err = "the arrow pass needs " + std::to_string(sizeof(ArrowPassConstants)) + " bytes of push constants";
    return false;
  }
  corner_fmt_ = find_corner_fmt(gpu);
  if (corner_fmt_ == nullptr) {
    err = "no integer vertex format for the arrow corners";
    return false;
  }
  const std::string vertex = vertex_source();
  const std::string fragment = fragment_source();

  std::array<struct pl_desc, 3> descs = {};
  const std::array<const char*, 3> names = {"geom", "attr", "palette"};
  for (size_t i = 0; i < descs.size(); ++i) {
    descs.at(i).name = names.at(i);
    descs.at(i).type = PL_DESC_SAMPLED_TEX;
    descs.at(i).binding = static_cast<int>(i);
  }
  struct pl_vertex_attrib attrib = {};
  attrib.name = "corner";
  attrib.fmt = corner_fmt_;
  attrib.offset = 0;
  attrib.location = 0;

  // Straight-alpha "over" into the premultiplied layer, as the cell passes (§10.5).
  struct pl_blend_params blend = {};
  blend.src_rgb = PL_BLEND_SRC_ALPHA;
  blend.dst_rgb = PL_BLEND_ONE_MINUS_SRC_ALPHA;
  blend.src_alpha = PL_BLEND_ONE;
  blend.dst_alpha = PL_BLEND_ONE_MINUS_SRC_ALPHA;

  struct pl_pass_params params = {};
  params.type = PL_PASS_RASTER;
  params.descriptors = descs.data();
  params.num_descriptors = static_cast<int>(descs.size());
  params.push_constants_size = sizeof(ArrowPassConstants);
  params.glsl_shader = fragment.c_str();
  params.vertex_type = PL_PRIM_TRIANGLE_LIST;
  params.vertex_attribs = &attrib;
  params.num_vertex_attribs = 1;
  params.vertex_stride = corner_fmt_->texel_size;
  params.vertex_shader = vertex.c_str();
  params.target_format = target_format;
  params.blend_params = &blend;
  params.load_target = true;
  pass_ = pl_pass_create(gpu, &params);
  if (pass_ == nullptr) {
    err = "pl_pass_create failed for the arrow pass";
    return false;
  }
  return true;
}

// Step 2: the static corner buffer, corner = vertex % 9, grown (never shrunk) to the largest draw.
bool ArrowPass::ensure_corners(pl_gpu gpu, size_t vertices, std::string& err) {
  if (corners_ != nullptr && corner_capacity_ >= vertices) {
    return true;
  }
  const size_t cap = std::max(vertices, corner_capacity_ * 2);
  const size_t ts = corner_fmt_->texel_size;
  if (cap * ts > gpu->limits.max_vbo_size) {
    err = "arrow corner buffer of " + std::to_string(cap * ts) + " bytes exceeds max_vbo_size";
    return false;
  }
  std::vector<uint8_t> data(cap * ts, 0);
  for (size_t i = 0; i < cap; ++i) {
    data[i * ts] = static_cast<uint8_t>(i % kVerticesPerInstance);  // little-endian low byte of the texel
  }
  pl_buf_destroy(gpu, &corners_);
  struct pl_buf_params params = {};
  params.size = data.size();
  params.drawable = true;
  params.initial_data = data.data();
  corners_ = pl_buf_create(gpu, &params);
  if (corners_ == nullptr) {
    corner_capacity_ = 0;
    err = "pl_buf_create failed for the arrow corner buffer";
    return false;
  }
  corner_capacity_ = cap;
  return true;
}

bool ArrowPass::run(pl_gpu gpu, pl_tex target, pl_tex palette, ArrowPassConstants pc, uint32_t first, uint32_t end,
                    uint32_t mode, std::string& err) {
  if (end <= first || geom_ == nullptr) {
    return true;
  }
  const size_t vertices = static_cast<size_t>(end - first) * kVerticesPerInstance;
  if (!ensure_pass(gpu, target->params.format, err) || !ensure_corners(gpu, vertices, err)) {
    return false;
  }
  pc.first = static_cast<int32_t>(first);
  pc.mode = mode;
  pc.tex_w = geom_->params.w;
  std::array<struct pl_desc_binding, 3> bindings = {};
  const std::array<pl_tex, 3> texs = {geom_, attr_, palette};
  for (size_t i = 0; i < bindings.size(); ++i) {
    bindings.at(i).object = texs.at(i);
    bindings.at(i).address_mode = PL_TEX_ADDRESS_CLAMP;
    bindings.at(i).sample_mode = PL_TEX_SAMPLE_NEAREST;  // texelFetch only
  }
  struct pl_pass_run_params r = {};
  r.pass = pass_;
  r.desc_bindings = bindings.data();
  r.push_constants = &pc;
  r.target = target;
  r.vertex_buf = corners_;
  r.buf_offset = 0;
  r.vertex_count = static_cast<int>(vertices);
  pl_pass_run(gpu, &r);
  return true;
}

bool ArrowPass::has_work(uint32_t layers) const {
  if (full_ == nullptr) {
    return false;
  }
  const bool mvs = (layers & (bit(OverlayLayer::MvL0) | bit(OverlayLayer::MvL1) | bit(OverlayLayer::MvBi))) != 0 &&
                   full_->seg.intra > 0;
  const bool intra = (layers & bit(OverlayLayer::IntraDirs)) != 0 && full_->seg.end > full_->seg.intra;
  return mvs || intra;
}

bool ArrowPass::draw_intra(pl_gpu gpu, pl_tex target, pl_tex palette, uint32_t layers, const ArrowPassConstants& pc,
                           std::string& err) {
  // R-MV-1 applies to strokes too: nothing below 4 device px per cell.
  if ((layers & bit(OverlayLayer::IntraDirs)) == 0 || !has_work(layers) || 4.0F * pc.zoom < 4.0F) {
    return true;
  }
  if (!ensure_view(gpu, pc, err)) {
    return false;
  }
  return run(gpu, target, palette, pc, resident_seg_.intra, resident_seg_.end, 0, err);
}

bool ArrowPass::draw_mvs(pl_gpu gpu, pl_tex target, pl_tex palette, uint32_t layers, const ArrowPassConstants& pc,
                         std::string& err) {
  // R-MV-1 (§10.5.4): arrows only when a cell is at least 4 device px.
  if (!has_work(layers) || 4.0F * pc.zoom < 4.0F) {
    return true;
  }
  if (!ensure_view(gpu, pc, err)) {
    return false;
  }
  const bool l0 = (layers & bit(OverlayLayer::MvL0)) != 0;
  const bool l1 = (layers & bit(OverlayLayer::MvL1)) != 0;
  const bool bi = (layers & bit(OverlayLayer::MvBi)) != 0;
  const ArrowSegments& s = resident_seg_;
  const uint32_t skip = bi ? kModeSkipBi : 0;
  // §10.5.6 pass 9: L1, then L0, then bi. L1 [0, l0_bi) and L0 [l0_bi, intra) are adjacent: one draw when both are on.
  bool ok = true;
  if (l0 && l1) {
    ok = run(gpu, target, palette, pc, 0, s.intra, skip, err);
  } else if (l1) {
    ok = run(gpu, target, palette, pc, 0, s.l0_bi, skip, err);
  } else if (l0) {
    ok = run(gpu, target, palette, pc, s.l0_bi, s.intra, skip, err);
  }
  if (ok && bi) {
    ok = run(gpu, target, palette, pc, s.l1_bi, s.l0_uni, kModeBiDraw, err);
  }
  return ok;
}

void ArrowPass::destroy(pl_gpu gpu) {
  drop_textures(gpu);
  pl_buf_destroy(gpu, &corners_);
  corner_capacity_ = 0;
  pl_pass_destroy(gpu, &pass_);
  corner_fmt_ = nullptr;
  full_ = nullptr;
}

}  // namespace ovt::render::detail
