// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.5 false colour and §10.7.8 gamut-clip overlay (WI-4.453). Q carries max(R,G,B), CIE Y and the
// out-of-gamut flags of every pixel (measure_prep.frag); false_color.frag turns it into an sRGB-encoded RGBA8 picture
// of the crop, which the surface presents in place of the decoded picture with the ViewState zoom and pan. The CPU
// helpers here state the same tables for the tests.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/custom.h>

#include "measure/measure.h"
#include "measure_shaders.h"  // generated from render/src/shaders/ (render/CMakeLists.txt)

namespace ovt::render::detail {

namespace {
constexpr double kSdrWhiteNits = 203.0;  // PL_COLOR_SDR_WHITE: M = 1.0
constexpr double kPqPeakNits = 10000.0;
constexpr double kTintAlpha = 153.0 / 255.0;  // 0x99

uint8_t to_code(double v) {
  return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}
}  // namespace

AnalysisView analysis_view(const ViewState& view) {
  // Both replace the picture; false colour wins when both are set.
  if (view.false_color) {
    return AnalysisView::FalseColor;
  }
  return view.gamut_clip_overlay ? AnalysisView::GamutClip : AnalysisView::None;
}

int false_color_band(double nits) {
  constexpr std::array<double, kFalseColorBands - 1> kEdges = {1.0, 100.0, 203.0, 1000.0, 4000.0};
  int band = 0;
  for (const double edge : kEdges) {
    if (nits >= edge) {
      ++band;
    }
  }
  return band;
}

std::array<uint8_t, 3> false_color_rgb(double nits) {
  switch (false_color_band(nits)) {
    case 0:
      return {0x1F, 0x1F, 0x5C};
    case 1: {
      const double t = std::clamp(std::log(nits) / std::log(100.0), 0.0, 1.0);
      const uint8_t g = to_code((64.0 + (128.0 * t)) / 255.0);
      return {g, g, g};
    }
    case 2:
      return {0x2E, 0xCC, 0x40};
    case 3:
      return {0xFF, 0xDC, 0x00};
    case 4:
      return {0xFF, 0x85, 0x1B};
    default:
      return {0xFF, 0x41, 0x36};
  }
}

std::array<uint8_t, 3> gamut_clip_rgb(double y_nits, bool outside_709, bool outside_p3) {
  const double grey = pq_oetf(std::max(y_nits, 0.0));
  std::array<double, 3> tint = {grey, grey, grey};
  if (outside_p3) {
    tint = {0.0, 1.0, 1.0};
  } else if (outside_709) {
    tint = {1.0, 0.0, 1.0};
  }
  const bool tinted = outside_p3 || outside_709;
  std::array<uint8_t, 3> out{};
  for (size_t c = 0; c < 3; ++c) {
    const double v = tinted ? grey + ((tint.at(c) - grey) * kTintAlpha) : grey;
    out.at(c) = to_code(v);
  }
  return out;
}

float gamut_peak(const struct pl_color_space& source) {
  // SPEC-ASSUMPTION(#490): the "luminance-relative maximum" of §10.7.8 is the container's peak: SDR reference
  // white (M = 1.0), the HLG Lw used for linearisation, or 10000 nits for PQ (whatever the mastering display was).
  struct pl_color_space c = measurement_source_color(source);
  pl_color_space_infer(&c);
  const double peak = c.transfer == PL_COLOR_TRC_PQ ? kPqPeakNits : static_cast<double>(c.hdr.max_luma);
  return static_cast<float>(std::max(peak, kSdrWhiteNits) / kSdrWhiteNits);
}

std::array<float, 9> gamut_matrix(enum pl_color_primaries from, enum pl_color_primaries to) {
  // Relative colorimetric: a plain 3x3 through XYZ (with a Bradford white adaptation only for non-D65 primaries).
  const pl_matrix3x3 m = pl_get_color_mapping_matrix(pl_raw_primaries_get(from), pl_raw_primaries_get(to),
                                                     PL_INTENT_RELATIVE_COLORIMETRIC);
  std::array<float, 9> col{};  // GLSL mat3 is column-major
  for (size_t r = 0; r < 3; ++r) {
    for (size_t c = 0; c < 3; ++c) {
      col.at((c * 3) + r) = m.m[r][c];
    }
  }
  return col;
}

bool analysis_pass(pl_dispatch dp, pl_gpu gpu, pl_tex q, AnalysisView mode, pl_tex* out, std::string& err) {
  struct pl_tex_params tp = {};
  tp.w = q->params.w;
  tp.h = q->params.h;
  tp.format = pl_find_named_fmt(gpu, "rgba8");
  tp.sampleable = true;
  tp.renderable = true;
  if (tp.format == nullptr || !pl_tex_recreate(gpu, out, &tp)) {
    err = "analysis view: cannot create the RGBA8 view texture";
    return false;
  }

  const int mode_value = mode == AnalysisView::GamutClip ? 1 : 0;
  pl_shader sh = pl_dispatch_begin(dp);
  struct pl_shader_desc desc = {};
  desc.desc.name = "ovt_q";
  desc.desc.type = PL_DESC_SAMPLED_TEX;
  desc.binding.object = q;
  desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
  struct pl_shader_var var = {};
  var.var = pl_var_int("ovt_mode");
  var.data = &mode_value;
  var.dynamic = true;
  struct pl_custom_shader custom = {};
  custom.header = static_cast<const char*>(kShaderFalseColor);
  custom.body = "color = ovt_false_color();";
  custom.input = PL_SHADER_SIG_NONE;
  custom.output = PL_SHADER_SIG_COLOR;
  custom.descriptors = &desc;
  custom.num_descriptors = 1;
  custom.variables = &var;
  custom.num_variables = 1;
  custom.output_w = tp.w;
  custom.output_h = tp.h;
  custom.description = "ovt false_color (§10.7.5, §10.7.8)";
  if (!pl_shader_custom(sh, &custom)) {
    pl_dispatch_abort(dp, &sh);
    err = "analysis view: false_color shader rejected";
    return false;
  }
  struct pl_dispatch_params dparams = {};
  dparams.shader = &sh;
  dparams.target = *out;
  if (!pl_dispatch_finish(dp, &dparams)) {
    err = "analysis view: false_color pass failed";
    return false;
  }
  return true;
}

}  // namespace ovt::render::detail
