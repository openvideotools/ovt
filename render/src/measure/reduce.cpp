// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// §10.7.2 per-pixel quantities and the §10.7.3 MaxCLL / FALL / min reduction chain (WI-4.452). Both are fragment
// passes through pl_dispatch with the GLSL of render/src/shaders/{measure_prep,reduce8}.frag. The reduction is exact
// over every pixel of the crop: no subsampling (§10.7.3).

#include <array>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/custom.h>

#include "measure/measure.h"
#include "measure_shaders.h"  // generated from render/src/shaders/ (render/CMakeLists.txt)

namespace ovt::render::detail {

std::vector<std::array<int, 2>> reduction_chain(int width, int height) {
  std::vector<std::array<int, 2>> chain;
  if (width <= 0 || height <= 0) {
    return chain;
  }
  constexpr int kStep = static_cast<int>(kReduceStep);
  int w = width;
  int h = height;
  do {
    w = (w + kStep - 1) / kStep;
    h = (h + kStep - 1) / kStep;
    chain.push_back({w, h});
  } while (w > 1 || h > 1);
  return chain;
}

bool prep_pass(pl_dispatch dp, pl_gpu gpu, pl_tex m, enum pl_color_primaries primaries, pl_tex* q, std::string& err) {
  struct pl_tex_params tp = {};
  tp.w = m->params.w;
  tp.h = m->params.h;
  tp.format = pl_find_named_fmt(gpu, "rgba16hf");
  tp.sampleable = true;
  tp.renderable = true;
  if (tp.format == nullptr || !pl_tex_recreate(gpu, q, &tp)) {
    err = "measurement: cannot create the RGBA16F quantity texture Q";
    return false;
  }

  // CIE Y of linear content RGB: the Y row of the primaries' RGB -> XYZ matrix (§10.6.3).
  const pl_matrix3x3 rgb2xyz = pl_get_rgb2xyz_matrix(pl_raw_primaries_get(primaries));
  const std::array<float, 3> luma = {rgb2xyz.m[1][0], rgb2xyz.m[1][1], rgb2xyz.m[1][2]};

  pl_shader sh = pl_dispatch_begin(dp);
  struct pl_shader_desc desc = {};
  desc.desc.name = "ovt_m";
  desc.desc.type = PL_DESC_SAMPLED_TEX;
  desc.binding.object = m;
  desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
  struct pl_shader_var var = {};
  var.var = pl_var_vec3("ovt_luma");
  var.data = luma.data();
  struct pl_custom_shader custom = {};
  custom.header = static_cast<const char*>(kShaderMeasurePrep);
  custom.body = "color = ovt_measure_prep();";
  custom.input = PL_SHADER_SIG_NONE;
  custom.output = PL_SHADER_SIG_COLOR;
  custom.descriptors = &desc;
  custom.num_descriptors = 1;
  custom.variables = &var;
  custom.num_variables = 1;
  custom.output_w = tp.w;
  custom.output_h = tp.h;
  custom.description = "ovt measure_prep (§10.7.2)";
  if (!pl_shader_custom(sh, &custom)) {
    pl_dispatch_abort(dp, &sh);
    err = "measurement: measure_prep shader rejected";
    return false;
  }
  struct pl_dispatch_params dparams = {};
  dparams.shader = &sh;
  dparams.target = *q;
  if (!pl_dispatch_finish(dp, &dparams)) {
    err = "measurement: measure_prep pass failed";
    return false;
  }
  return true;
}

void destroy_levels(pl_gpu gpu, std::vector<pl_tex>& levels) {
  for (pl_tex& t : levels) {
    pl_tex_destroy(gpu, &t);
  }
  levels.clear();
}

bool reduce_chain(pl_dispatch dp, pl_gpu gpu, pl_tex q, std::vector<pl_tex>& levels, std::string& err) {
  const std::vector<std::array<int, 2>> chain = reduction_chain(q->params.w, q->params.h);
  if (levels.size() > chain.size()) {
    for (size_t i = chain.size(); i < levels.size(); ++i) {
      pl_tex_destroy(gpu, &levels[i]);
    }
  }
  levels.resize(chain.size(), nullptr);

  pl_fmt fmt = pl_find_named_fmt(gpu, "rgba32f");
  pl_tex src = q;
  for (size_t i = 0; i < chain.size(); ++i) {
    const bool last = i + 1 == chain.size();
    struct pl_tex_params tp = {};
    tp.w = chain[i][0];
    tp.h = chain[i][1];
    tp.format = fmt;
    tp.sampleable = !last;
    tp.renderable = true;
    tp.host_readable = last;
    if (fmt == nullptr || !pl_tex_recreate(gpu, &levels[i], &tp)) {
      err = "measurement: cannot create an RGBA32F reduction texture";
      return false;
    }

    const int first = i == 0 ? 1 : 0;
    pl_shader sh = pl_dispatch_begin(dp);
    struct pl_shader_desc desc = {};
    desc.desc.name = "ovt_src";
    desc.desc.type = PL_DESC_SAMPLED_TEX;
    desc.binding.object = src;
    desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    struct pl_shader_var var = {};
    var.var = pl_var_int("ovt_first");
    var.data = &first;
    var.dynamic = true;
    struct pl_custom_shader custom = {};
    custom.header = static_cast<const char*>(kShaderReduce8);
    custom.body = "color = ovt_reduce8();";
    custom.input = PL_SHADER_SIG_NONE;
    custom.output = PL_SHADER_SIG_COLOR;
    custom.descriptors = &desc;
    custom.num_descriptors = 1;
    custom.variables = &var;
    custom.num_variables = 1;
    custom.output_w = tp.w;
    custom.output_h = tp.h;
    custom.description = "ovt reduce8 (§10.7.3)";
    if (!pl_shader_custom(sh, &custom)) {
      pl_dispatch_abort(dp, &sh);
      err = "measurement: reduce8 shader rejected";
      return false;
    }
    struct pl_dispatch_params dparams = {};
    dparams.shader = &sh;
    dparams.target = levels[i];
    if (!pl_dispatch_finish(dp, &dparams)) {
      err = "measurement: reduce8 pass failed";
      return false;
    }
    src = levels[i];
  }
  return true;
}

}  // namespace ovt::render::detail
