// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ViewState -> pl_render_params and the target pl_frame (chapter 10 §10.3.1, §10.3.2, §10.3.4, §10.3.5; WI-1.403).

#ifndef OVT_RENDER_SRC_PIPELINE_RENDER_PARAMS_H
#define OVT_RENDER_SRC_PIPELINE_RENDER_PARAMS_H

#include <libplacebo/colorspace.h>
#include <libplacebo/renderer.h>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

// The §10.3.2 row a target renders to: an offscreen target by its format (Rgba8Srgb -> Srgb8, Rgba16F ->
// ScRgbLinear16F, Rgb10A2 -> Pq10), a native target by DisplayTarget.encoding.
OVT_RENDER_API DisplayTarget::OutputEncoding target_encoding(const DisplayTarget& target);

// §10.3.2 target colour space of `encoding` before any ViewState override (primaries, transfer, hdr.max_luma).
OVT_RENDER_API struct pl_color_space target_color(const DisplayTarget& target, DisplayTarget::OutputEncoding encoding);

// §10.3.2 target.repr.bits.color_depth: 8, 16 (float), 10, 10.
OVT_RENDER_API int target_color_depth(DisplayTarget::OutputEncoding encoding);

// True for the float encoding, which never dithers (§10.3.5; libplacebo asserts on dither_params for float targets).
OVT_RENDER_API bool is_float_encoding(DisplayTarget::OutputEncoding encoding);

// §10.2.3 viewport mapping of the source rectangle: device = (image - pan) * zoom + viewport / 2.
OVT_RENDER_API pl_rect2df target_rect(const pl_rect2df& source, const ViewState& view, int width, int height);

// One render's parameters. pl_render_params points at the other members, so the set is neither copied nor moved:
// build() fills it in place for the render that follows.
class OVT_RENDER_API RenderParamSet {
 public:
  RenderParamSet() = default;
  RenderParamSet(const RenderParamSet&) = delete;
  RenderParamSet& operator=(const RenderParamSet&) = delete;
  RenderParamSet(RenderParamSet&&) = delete;
  RenderParamSet& operator=(RenderParamSet&&) = delete;
  ~RenderParamSet() = default;

  // Fills params from `view` (§10.3.1 filters, §10.3.4 tone and gamut mapping, §10.3.5 dithering) and adjusts
  // target.color.hdr.max_luma for target_peak_override, ToneMap::Clip and ToneMap::None. `source` is the image's
  // colour space; `offscreen` disables peak detection even for ToneMap::Auto (R-TM-1, FR-PLN-101).
  void build(const ViewState& view, DisplayTarget::OutputEncoding encoding, bool offscreen,
             const struct pl_color_space& source, struct pl_frame& target);

  [[nodiscard]] const struct pl_render_params& params() const { return params_; }

 private:
  struct pl_render_params params_ = {};
  struct pl_color_map_params cmap_ = {};
  struct pl_dither_params dither_ = {};
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PIPELINE_RENDER_PARAMS_H
