// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ViewState -> pl_render_params and the target pl_frame (chapter 10 §10.3.1, §10.3.2, §10.3.4, §10.3.5; WI-1.403).

#include "pipeline/render_params.h"

#include <libplacebo/filters.h>
#include <libplacebo/gamut_mapping.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/shaders/dithering.h>
#include <libplacebo/tone_mapping.h>

namespace ovt::render::detail {
namespace {

const struct pl_gamut_map_function* gamut_function(GamutMap g) {
  switch (g) {
    case GamutMap::Relative:
      return &pl_gamut_map_relative;
    case GamutMap::Saturation:
      return &pl_gamut_map_saturation;
    case GamutMap::Absolute:
      return &pl_gamut_map_absolute;
    case GamutMap::Desaturate:
      return &pl_gamut_map_desaturate;
    case GamutMap::Darken:
      return &pl_gamut_map_darken;
    case GamutMap::Clip:
      return &pl_gamut_map_clip;
    case GamutMap::Highlight:
      return &pl_gamut_map_highlight;
    case GamutMap::Perceptual:
      break;
  }
  return &pl_gamut_map_perceptual;
}

const struct pl_filter_config* downscale_filter(Downscale d) {
  switch (d) {
    case Downscale::Box:
      return &pl_filter_box;
    case Downscale::Lanczos:
      return &pl_filter_lanczos;
    case Downscale::Bilinear:
      break;
  }
  return &pl_filter_bilinear;
}

// The source peak libplacebo would assume when the stream carries no mastering metadata (PQ 10000, HLG 1000, SDR 203).
float source_peak(const struct pl_color_space& source) {
  struct pl_color_space inferred = source;
  pl_color_space_infer(&inferred);
  return inferred.hdr.max_luma;
}

}  // namespace

DisplayTarget::OutputEncoding target_encoding(const DisplayTarget& target) {
  if (target.kind != DisplayTarget::Kind::OffscreenTexture) {
    return target.encoding;
  }
  switch (target.offscreen_format) {
    case DisplayTarget::OffscreenFormat::Rgba16F:
      return DisplayTarget::OutputEncoding::ScRgbLinear16F;
    case DisplayTarget::OffscreenFormat::Rgb10A2:
      return DisplayTarget::OutputEncoding::Pq10;
    case DisplayTarget::OffscreenFormat::Rgba8Srgb:
      break;
  }
  return DisplayTarget::OutputEncoding::Srgb8;
}

struct pl_color_space target_color(const DisplayTarget& target, DisplayTarget::OutputEncoding encoding) {
  struct pl_color_space c = pl_color_space_unknown;
  // Headroom above 1 describes an HDR output whose peak the platform reported (§10.4.5).
  const float display_peak = target.headroom * target.sdr_white_nits;
  const bool peak_known = target.headroom > 1.0F && display_peak > 0.0F;
  switch (encoding) {
    case DisplayTarget::OutputEncoding::Srgb8:
      c.primaries = PL_COLOR_PRIM_BT_709;
      c.transfer = PL_COLOR_TRC_SRGB;
      c.hdr.max_luma = PL_COLOR_SDR_WHITE;
      break;
    case DisplayTarget::OutputEncoding::ScRgbLinear16F:
      c.primaries = PL_COLOR_PRIM_BT_709;
      c.transfer = PL_COLOR_TRC_LINEAR;
      c.hdr.max_luma = display_peak;
      break;
    case DisplayTarget::OutputEncoding::Pq10:
      c.primaries = PL_COLOR_PRIM_BT_2020;
      c.transfer = PL_COLOR_TRC_PQ;
      c.hdr.max_luma = peak_known ? display_peak : 1000.0F;
      break;
    case DisplayTarget::OutputEncoding::Hlg10:
      c.primaries = PL_COLOR_PRIM_BT_2020;
      c.transfer = PL_COLOR_TRC_HLG;
      c.hdr.max_luma = 1000.0F;
      break;
  }
  return c;
}

int target_color_depth(DisplayTarget::OutputEncoding encoding) {
  switch (encoding) {
    case DisplayTarget::OutputEncoding::ScRgbLinear16F:
      return 16;
    case DisplayTarget::OutputEncoding::Pq10:
    case DisplayTarget::OutputEncoding::Hlg10:
      return 10;
    case DisplayTarget::OutputEncoding::Srgb8:
      break;
  }
  return 8;
}

bool is_float_encoding(DisplayTarget::OutputEncoding encoding) {
  return encoding == DisplayTarget::OutputEncoding::ScRgbLinear16F;
}

pl_rect2df target_rect(const pl_rect2df& source, const ViewState& view, int width, int height) {
  const double zoom = view.zoom > 0.0 ? view.zoom : 1.0;
  const double half_w = width / 2.0;
  const double half_h = height / 2.0;
  auto map = [zoom](float v, double pan, double half) {
    return static_cast<float>(((static_cast<double>(v) - pan) * zoom) + half);
  };
  return pl_rect2df{map(source.x0, view.pan_x, half_w), map(source.y0, view.pan_y, half_h),
                    map(source.x1, view.pan_x, half_w), map(source.y1, view.pan_y, half_h)};
}

void RenderParamSet::build(const ViewState& view, DisplayTarget::OutputEncoding encoding, bool offscreen,
                           const struct pl_color_space& source, struct pl_frame& target) {
  params_ = pl_render_default_params;

  // §10.3.1 filters: nearest-neighbour from 1:1 upwards (pixel-exact inspection), the ViewState downscaler below.
  const double zoom = view.zoom > 0.0 ? view.zoom : 1.0;
  params_.upscaler = zoom >= 1.0 ? &pl_filter_nearest : &pl_filter_bilinear;
  params_.downscaler = downscale_filter(view.downscale);
  params_.plane_upscaler = view.chroma_upscale == ChromaUpscale::Nearest ? &pl_filter_nearest : &pl_filter_bilinear;
  params_.skip_anti_aliasing = zoom >= 1.0;
  params_.antiringing_strength = 0.0F;
  // The surface clears the target to background_rgba first; libplacebo must not clear the border again.
#if PL_API_VER >= 346
  params_.border = PL_CLEAR_SKIP;
#else
  params_.skip_target_clearing = true;
#endif

  // §10.3.2 target frame.
  target.repr = pl_color_repr_rgb;
  target.repr.bits.color_depth = target_color_depth(encoding);
  if (view.target_peak_override > 0.0F) {
    target.color.hdr.max_luma = view.target_peak_override;
  }

  // §10.3.4 tone and gamut mapping.
  cmap_ = pl_color_map_default_params;
  cmap_.metadata = PL_HDR_METADATA_ANY;
  cmap_.contrast_recovery = 0.0F;
  params_.peak_detect_params = nullptr;  // R-TM-1: static and deterministic unless Auto on a live display
  switch (view.tone_map) {
    case ToneMap::Auto:
      cmap_.tone_mapping_function = &pl_tone_map_spline;
      cmap_.contrast_recovery = 0.3F;
      if (!offscreen) {
        params_.peak_detect_params = &pl_peak_detect_default_params;
      }
      break;
    case ToneMap::BT2390:
      cmap_.tone_mapping_function = &pl_tone_map_bt2390;
      break;
    case ToneMap::BT2446A:
      cmap_.tone_mapping_function = &pl_tone_map_bt2446a;
      break;
    case ToneMap::Spline:
      cmap_.tone_mapping_function = &pl_tone_map_spline;
      break;
    case ToneMap::Clip:
      cmap_.tone_mapping_function = &pl_tone_map_clip;
      target.color.hdr.max_luma = view.clip_nits;
      break;
    case ToneMap::None:
      cmap_.tone_mapping_function = &pl_tone_map_clip;
      target.color.hdr.max_luma = source_peak(source);
      break;
  }
  // §10.7.8: the gamut clip overlay is pl_gamut_map_highlight.
  cmap_.gamut_mapping = view.gamut_clip_overlay ? &pl_gamut_map_highlight : gamut_function(view.gamut_map);
  params_.color_map_params = &cmap_;

  // §10.3.5 dithering: blue noise at the target depth; never for float targets or with ViewState.dither off.
  if (view.dither && !is_float_encoding(encoding)) {
    dither_ = pl_dither_default_params;
    dither_.method = PL_DITHER_BLUE_NOISE;
    dither_.lut_size = 6;
    dither_.temporal = false;
    params_.dither_params = &dither_;
  } else {
    params_.dither_params = nullptr;
  }
}

}  // namespace ovt::render::detail
