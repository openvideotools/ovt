// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The 1-px sample grid of R-ZOOM-4 (chapter 10 §10.6.2, WI-1.409) as a pl_overlay over the picture: the lines come
// from view_transform.h, the layer is a drawable-size premultiplied RGBA8 texture that is re-uploaded only when the
// view, the picture rectangle or the viewport changes. Internal to libovt-render; render thread only.

#ifndef OVT_RENDER_SRC_PIPELINE_SAMPLE_GRID_H
#define OVT_RENDER_SRC_PIPELINE_SAMPLE_GRID_H

#include <string>

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

class SampleGridLayer {
 public:
  SampleGridLayer() = default;
  SampleGridLayer(const SampleGridLayer&) = delete;
  SampleGridLayer& operator=(const SampleGridLayer&) = delete;
  SampleGridLayer(SampleGridLayer&&) = delete;
  SampleGridLayer& operator=(SampleGridLayer&&) = delete;
  ~SampleGridLayer() = default;  // destroy(gpu) frees the texture; the owner calls it before the pl_gpu goes

  // Fills `out` for pl_frame.overlays when `view.zoom` >= 8 and a grid line falls inside `picture` (device rectangle
  // of the drawn picture, `source` its luma-sample rectangle). Returns false when nothing is to be drawn, or on error
  // (`err` set).
  bool draw(pl_gpu gpu, const ViewState& view, int w, int h, const pl_rect2df& source, const pl_rect2df& picture,
            struct pl_overlay& out, std::string& err);

  void destroy(pl_gpu gpu);

 private:
  pl_tex tex_ = nullptr;
  struct pl_overlay_part part_ = {};
  // What the texture holds: the inputs of the last upload.
  double zoom_ = 0, pan_x_ = 0, pan_y_ = 0;  // NOLINT(readability-isolate-declaration)
  int w_ = 0, h_ = 0;                        // NOLINT(readability-isolate-declaration)
  pl_rect2df source_{}, picture_{};
  bool valid_ = false;
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PIPELINE_SAMPLE_GRID_H
