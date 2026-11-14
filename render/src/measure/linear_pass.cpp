// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// §10.7.1 linear-light texture M (WI-4.452): picture A rendered once more, 1:1 over its conformance window, into an
// RGBA16F texture in linear light with the content primaries. The target peak (10000 nits, or Lw for HLG) is at least
// the source peak and the clip tone-mapping function leaves in-range values alone, so libplacebo does not tone-map:
// M = linear light / 203 (PL_COLOR_SDR_WHITE).

#include <cmath>
#include <string>

#include <libplacebo/colorspace.h>
#include <libplacebo/filters.h>
#include <libplacebo/gamut_mapping.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/tone_mapping.h>

#include "measure/measure.h"

namespace ovt::render::detail {

namespace {
constexpr float kMeasurePeakNits = 10000.0F;
constexpr float kHlgDefaultPeakNits = 1000.0F;  // BT.2100 reference Lw when the stream carries no MDCV
}  // namespace

struct pl_color_space measurement_source_color(const struct pl_color_space& source) {
  struct pl_color_space c = source;
  // §10.7.1 and the WI-4.452 pitfall: HLG without mastering metadata is linearised at Lw = 1000. libplacebo infers
  // the same peak today (PL_COLOR_HLG_PEAK); setting it keeps the 75 % = 203 nits relation independent of that.
  if (c.transfer == PL_COLOR_TRC_HLG && !(c.hdr.max_luma > 0.0F)) {
    c.hdr.max_luma = kHlgDefaultPeakNits;
  }
  return c;
}

struct pl_color_space measurement_target_color(const struct pl_color_space& source) {
  struct pl_color_space inferred = measurement_source_color(source);
  pl_color_space_infer(&inferred);
  struct pl_color_space c = pl_color_space_unknown;
  c.primaries = inferred.primaries;
  c.transfer = PL_COLOR_TRC_LINEAR;
  // SPEC-ASSUMPTION(#473): HLG: libplacebo tunes an HLG source's OOTF to the peak of an HDR target
  // (pl_color_space_infer_map), so the target peak is the source's Lw (1000 without MDCV); the HLG output never exceeds
  // Lw, so nothing is tone-mapped either.
  c.hdr.max_luma = inferred.transfer == PL_COLOR_TRC_HLG ? inferred.hdr.max_luma : kMeasurePeakNits;
  // SPEC-ASSUMPTION(#473): libplacebo infers an SDR-like 1000:1 black (10 nits at a 10000-nit peak) for a linear target
  // without a black level and would lift the source black towards it; the source's own black level keeps the mapping an
  // identity.
  c.hdr.min_luma = inferred.hdr.min_luma;
  return c;
}

bool render_linear(pl_gpu gpu, pl_renderer renderer, const FrameSlot& slot, pl_tex* m, std::string& err) {
  const pl_rect2df crop = source_rect(slot, true);
  const int width = static_cast<int>(std::lround(std::fabs(crop.x1 - crop.x0)));
  const int height = static_cast<int>(std::lround(std::fabs(crop.y1 - crop.y0)));
  if (width <= 0 || height <= 0) {
    err = "measurement: empty picture crop";
    return false;
  }

  pl_fmt fmt = pl_find_named_fmt(gpu, "rgba16hf");
  struct pl_tex_params tp = {};
  tp.w = width;
  tp.h = height;
  tp.format = fmt;
  tp.sampleable = true;
  tp.renderable = true;
  if (fmt == nullptr || !pl_tex_recreate(gpu, m, &tp)) {
    err = "measurement: cannot create the RGBA16F measurement texture";
    return false;
  }

  struct pl_frame image = slot.frame;
  image.crop = crop;
  image.color = measurement_source_color(image.color);

  struct pl_frame target = {};
  target.num_planes = 1;
  target.planes[0].texture = *m;
  target.planes[0].components = 4;
  for (int c = 0; c < 4; ++c) {
    target.planes[0].component_mapping[c] = c;
  }
  target.repr = pl_color_repr_rgb;
  // repr.bits stays zero: libplacebo takes the depth from the float format (a declared depth rescales the output).
  target.repr.bits = {};
  target.color = measurement_target_color(slot.frame.color);
  target.crop = pl_rect2df{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};

  // §10.7.1: box filters (no scaling happens at 1:1), clip tone and gamut mapping, no dithering, no peak detection,
  // no debanding or other enhancement.
  struct pl_color_map_params cmap = pl_color_map_default_params;
  cmap.tone_mapping_function = &pl_tone_map_clip;
  cmap.gamut_mapping = &pl_gamut_map_clip;
  cmap.metadata = PL_HDR_METADATA_ANY;
  cmap.contrast_recovery = 0.0F;
  cmap.inverse_tone_mapping = false;
  struct pl_render_params params = pl_render_default_params;
  params.upscaler = &pl_filter_box;
  params.downscaler = &pl_filter_box;
  params.plane_upscaler = &pl_filter_box;
  params.plane_downscaler = &pl_filter_box;
  params.antiringing_strength = 0.0F;
  params.skip_anti_aliasing = true;
  params.deband_params = nullptr;
  params.sigmoid_params = nullptr;
  params.color_adjustment = nullptr;
  params.peak_detect_params = nullptr;
  params.dither_params = nullptr;
  params.color_map_params = &cmap;
  if (!pl_render_image(renderer, &image, &target, &params)) {
    err = "measurement: pl_render_image into the measurement texture failed";
    return false;
  }
  return true;
}

}  // namespace ovt::render::detail
