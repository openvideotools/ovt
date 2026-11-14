// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The overlay layer of chapter 10 §10.5 (WI-2.431): a drawable-size RGBA8 sRGB pl_tex, cleared to transparent and
// drawn by the overlay passes in §10.5.6 order, then composited by libplacebo as pl_frame.overlays[0] so that its
// white lands at SDR reference white on every target (R-OVL-1, FR-HDR-014). It also owns the GPU copies of an
// OverlaySet's cell textures and the five static heatmap LUT textures. Internal to libovt-render; render thread only.

#ifndef OVT_RENDER_SRC_OVERLAY_OVERLAY_LAYER_H
#define OVT_RENDER_SRC_OVERLAY_OVERLAY_LAYER_H

#include <array>
#include <cstdint>
#include <string>

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "overlay/overlay_set_impl.h"
#include "overlay/pass_fill.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

// The fill palette (§10.5.2), RGBA (straight alpha) per row with the fill alpha where a row has one. Rows 0-29 are
// the overlay rows; frame-type rows 30-32 are opaque. WI-2.439 replaces it with kOverlayColors / kOverlayColorsCvd.
OVT_RENDER_API extern const std::array<uint32_t, 33> kFillPalette;

class OverlayLayerGpu {
 public:
  explicit OverlayLayerGpu(pl_gpu gpu) : gpu_(gpu) {}
  OverlayLayerGpu(const OverlayLayerGpu&) = delete;
  OverlayLayerGpu& operator=(const OverlayLayerGpu&) = delete;
  OverlayLayerGpu(OverlayLayerGpu&&) = delete;
  OverlayLayerGpu& operator=(OverlayLayerGpu&&) = delete;
  ~OverlayLayerGpu();

  // Uploads the cell textures of `set` (set_overlays()). nullptr drops them. Returns false with `err` set when a
  // texture cannot be created; nothing is drawn then.
  bool upload(const OverlaySet* set, std::string& err);

  // Draws passes 2-4 into a w x h layer for `view`, limited to `picture` (device-px rect of the drawn picture), and
  // fills `out` for pl_frame.overlays. Returns false when there is nothing to draw (no set, no enabled layer with
  // data) or on error (`err` set).
  bool draw(const ViewState& view, int w, int h, float dpr, const pl_rect2df& picture, struct pl_overlay& out,
            std::string& err);

 private:
  bool ensure_static(std::string& err);
  bool ensure_layer(int w, int h, std::string& err);
  void drop_set();

  pl_gpu gpu_;
  pl_tex layer_ = nullptr;
  pl_tex palette_ = nullptr;
  std::array<pl_tex, 5> luts_{};  // Lut enum order: Viridis, Turbo, Cividis (= viridis, #471), Jet, Hot
  pl_tex attr_ = nullptr;
  pl_tex heat_ = nullptr;
  pl_tex heat_valid_ = nullptr;
  OverlayConfig cfg_{};
  uint32_t cells_w_ = 0;
  uint32_t cells_h_ = 0;
  CellPasses passes_;
  struct pl_overlay_part part_ = {};
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_OVERLAY_LAYER_H
