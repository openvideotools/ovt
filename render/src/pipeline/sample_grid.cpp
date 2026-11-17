// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The 1-px sample grid of R-ZOOM-4 (WI-1.409). See sample_grid.h.

#include "pipeline/sample_grid.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "pipeline/view_transform.h"

namespace ovt::render::detail {

namespace {

bool same_rect(const pl_rect2df& a, const pl_rect2df& b) {
  return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

}  // namespace

bool SampleGridLayer::draw(pl_gpu gpu, const ViewState& view, int w, int h, const pl_rect2df& source,
                           const pl_rect2df& picture, struct pl_overlay& out, std::string& err) {
  const double zoom = effective_zoom(view);
  if (!sample_grid_visible(zoom) || w <= 0 || h <= 0 || gpu == nullptr) {
    return false;
  }
  if (!valid_ || tex_ == nullptr || zoom != zoom_ || view.pan_x != pan_x_ || view.pan_y != pan_y_ || w != w_ ||
      h != h_ || !same_rect(source, source_) || !same_rect(picture, picture_)) {
    const ViewportSize vp{w, h};
    const SampleGridLines lines = sample_grid_lines(view, vp, source, picture);
    if (lines.columns.empty() && lines.rows.empty()) {
      valid_ = false;
      return false;
    }
    const std::vector<uint8_t> px = rasterize_sample_grid(lines, vp, picture);
    pl_fmt fmt = pl_find_named_fmt(gpu, "rgba8");
    if (fmt == nullptr || !(fmt->caps & PL_FMT_CAP_SAMPLEABLE)) {
      err = "sample grid format rgba8 is not sampleable";
      return false;
    }
    struct pl_tex_params params = {};
    params.w = w;
    params.h = h;
    params.format = fmt;
    params.sampleable = true;
    params.host_writable = true;
    if (!pl_tex_recreate(gpu, &tex_, &params)) {
      err = "pl_tex_recreate failed for the sample grid";
      valid_ = false;
      return false;
    }
    struct pl_tex_transfer_params up = {};
    up.tex = tex_;
    up.row_pitch = static_cast<size_t>(w) * 4;
    up.ptr = const_cast<uint8_t*>(
        px.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast): libplacebo's field is non-const
    if (!pl_tex_upload(gpu, &up)) {
      err = "pl_tex_upload failed for the sample grid";
      valid_ = false;
      return false;
    }
    zoom_ = zoom;
    pan_x_ = view.pan_x;
    pan_y_ = view.pan_y;
    w_ = w;
    h_ = h;
    source_ = source;
    picture_ = picture;
    valid_ = true;
  }
  part_ = {};
  part_.src = {0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h)};
  part_.dst = part_.src;
  out = {};
  out.tex = tex_;
  out.mode = PL_OVERLAY_NORMAL;
  out.coords = PL_OVERLAY_COORDS_DST_FRAME;
  out.repr = pl_color_repr_rgb;
  out.repr.levels = PL_COLOR_LEVELS_FULL;
  out.repr.alpha = PL_ALPHA_PREMULTIPLIED;
  out.color = pl_color_space_srgb;
  out.parts = &part_;
  out.num_parts = 1;
  return true;
}

void SampleGridLayer::destroy(pl_gpu gpu) {
  if (gpu != nullptr) {
    pl_tex_destroy(gpu, &tex_);
  }
  tex_ = nullptr;
  valid_ = false;
}

}  // namespace ovt::render::detail
