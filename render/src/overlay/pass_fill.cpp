// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Cell passes 2-4 of chapter 10 §10.5.6 (WI-2.431): heatmap, prediction / reference-index fill and skip hatch, each a
// full-viewport quad whose fragment shader maps its device pixel back to a 4x4 cell (§10.5.1 "Cell attribute textures
// are indexed by image position") and reads the cell textures with texelFetch.

#include "overlay/pass_fill.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

#include "shaders/fill_glsl.h"
#include "shaders/hatch_glsl.h"
#include "shaders/heatmap_glsl.h"

namespace ovt::render::detail {
namespace {

struct PassSpec {
  const char* body;
  std::array<const char*, 3> samplers;  // GLSL declarations, binding = index; nullptr = unused
};

PassSpec spec_of(CellPass pass) {
  switch (pass) {
    case CellPass::Heatmap:
      return {shaders::kHeatmap, {"sampler2D heat", "sampler2D heat_valid", "sampler2D lut"}};
    case CellPass::Fill:
      return {shaders::kFill, {"usampler2D cells", "sampler2D palette", nullptr}};
    case CellPass::Hatch:
      return {shaders::kHatch, {"usampler2D cells", "sampler2D palette", nullptr}};
  }
  return {shaders::kFill, {nullptr, nullptr, nullptr}};
}

// The push constant block, the output and ovt_cell(): device pixel -> image position (§10.2.3 viewport mapping,
// inverted) -> cell. Vulkan GLSL only: the VideoSurface runs on libplacebo's Vulkan backend (MoltenVK on macOS); a
// GLES backend adds its own prelude (uniforms instead of push constants) with the same names.
constexpr const char* kPrelude = R"glsl(
layout(std430, push_constant) uniform PushC {
  vec2 viewport_half;
  vec2 pan;
  vec4 clip;
  float zoom;
  float alpha;
  float dpr;
  uint layers;
  ivec2 cells;
} pc;

layout(location = 0) out vec4 out_color;

bool ovt_cell(out ivec2 cell) {
  vec2 p = gl_FragCoord.xy;  // pixel centre, origin top-left
  cell = ivec2(0);
  if (p.x < pc.clip.x || p.y < pc.clip.y || p.x >= pc.clip.z || p.y >= pc.clip.w) {
    return false;
  }
  vec2 image = (p - pc.viewport_half) / pc.zoom + pc.pan;
  cell = ivec2(floor(image / 4.0));
  return cell.x >= 0 && cell.y >= 0 && cell.x < pc.cells.x && cell.y < pc.cells.y;
}
)glsl";

constexpr const char* kVertexShader = R"glsl(#version 450
layout(location = 0) in vec2 pos;
void main() {
  gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

// Two triangles covering clip space.
constexpr std::array<float, 12> kQuad = {-1.0F, -1.0F, 1.0F, -1.0F, -1.0F, 1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 1.0F};

std::string fragment_source(const PassSpec& spec) {
  std::string src = "#version 450\n";
  for (size_t i = 0; i < spec.samplers.size(); ++i) {
    if (spec.samplers.at(i) != nullptr) {
      src += "layout(binding = " + std::to_string(i) + ") uniform " + spec.samplers.at(i) + ";\n";
    }
  }
  src += kPrelude;
  src += spec.body;
  return src;
}

pl_pass create_pass(pl_gpu gpu, CellPass pass, pl_fmt target_format, std::string& err) {
  if (!gpu->glsl.vulkan) {
    err = "overlay passes need libplacebo's Vulkan backend";
    return nullptr;
  }
  if (gpu->limits.max_pushc_size < sizeof(CellPassConstants)) {
    err = "overlay passes need " + std::to_string(sizeof(CellPassConstants)) + " bytes of push constants";
    return nullptr;
  }
  pl_fmt vertex_fmt = pl_find_vertex_fmt(gpu, PL_FMT_FLOAT, 2);
  if (vertex_fmt == nullptr) {
    err = "no vec2 vertex format";
    return nullptr;
  }
  const PassSpec spec = spec_of(pass);
  const std::string fragment = fragment_source(spec);

  std::vector<struct pl_desc> descs;
  for (size_t i = 0; i < spec.samplers.size(); ++i) {
    if (spec.samplers.at(i) != nullptr) {
      struct pl_desc d = {};
      d.name = spec.samplers.at(i);
      d.type = PL_DESC_SAMPLED_TEX;
      d.binding = static_cast<int>(i);
      descs.push_back(d);
    }
  }
  struct pl_vertex_attrib attrib = {};
  attrib.name = "pos";
  attrib.fmt = vertex_fmt;
  attrib.offset = 0;
  attrib.location = 0;

  // Straight-alpha fragments composited "over" the layer, which therefore holds premultiplied colour (§10.5:
  // pl_overlay.repr.alpha = PL_ALPHA_PREMULTIPLIED).
  struct pl_blend_params blend = {};
  blend.src_rgb = PL_BLEND_SRC_ALPHA;
  blend.dst_rgb = PL_BLEND_ONE_MINUS_SRC_ALPHA;
  blend.src_alpha = PL_BLEND_ONE;
  blend.dst_alpha = PL_BLEND_ONE_MINUS_SRC_ALPHA;

  struct pl_pass_params params = {};
  params.type = PL_PASS_RASTER;
  params.descriptors = descs.data();
  params.num_descriptors = static_cast<int>(descs.size());
  params.push_constants_size = sizeof(CellPassConstants);
  params.glsl_shader = fragment.c_str();
  params.vertex_type = PL_PRIM_TRIANGLE_LIST;
  params.vertex_attribs = &attrib;
  params.num_vertex_attribs = 1;
  params.vertex_stride = 2 * sizeof(float);
  params.vertex_shader = kVertexShader;
  params.target_format = target_format;
  params.blend_params = &blend;
  params.load_target = true;
  pl_pass p = pl_pass_create(gpu, &params);
  if (p == nullptr) {
    err = "pl_pass_create failed for an overlay pass";
  }
  return p;
}

}  // namespace

bool CellPasses::run(pl_gpu gpu, CellPass pass, pl_tex target, const CellPassConstants& constants,
                     std::initializer_list<pl_tex> textures, std::string& err) {
  pl_pass& p = passes_.at(static_cast<size_t>(pass));
  if (p == nullptr) {
    p = create_pass(gpu, pass, target->params.format, err);
    if (p == nullptr) {
      return false;
    }
  }
  std::vector<struct pl_desc_binding> bindings;
  bindings.reserve(textures.size());
  for (pl_tex t : textures) {
    struct pl_desc_binding b = {};
    b.object = t;
    b.address_mode = PL_TEX_ADDRESS_CLAMP;
    b.sample_mode = PL_TEX_SAMPLE_NEAREST;  // texelFetch only; integer textures cannot be filtered
    bindings.push_back(b);
  }
  CellPassConstants pc = constants;
  struct pl_pass_run_params run = {};
  run.pass = p;
  run.desc_bindings = bindings.data();
  run.push_constants = &pc;
  run.target = target;
  run.vertex_count = static_cast<int>(kQuad.size() / 2);
  run.vertex_data = kQuad.data();
  pl_pass_run(gpu, &run);
  return true;
}

void CellPasses::destroy(pl_gpu gpu) {
  for (pl_pass& p : passes_) {
    pl_pass_destroy(gpu, &p);
  }
}

}  // namespace ovt::render::detail
