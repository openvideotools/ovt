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
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "overlay/idbuffer.h"
#include "overlay/overlay_set_impl.h"
#include "overlay/pass_arrows.h"
#include "overlay/pass_fill.h"
#include "overlay/pass_lines.h"
#include "overlay/pass_misc.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

class OverlayLayerGpu {
 public:
  explicit OverlayLayerGpu(pl_gpu gpu) : gpu_(gpu), ids_(gpu) {}
  OverlayLayerGpu(const OverlayLayerGpu&) = delete;
  OverlayLayerGpu& operator=(const OverlayLayerGpu&) = delete;
  OverlayLayerGpu(OverlayLayerGpu&&) = delete;
  OverlayLayerGpu& operator=(OverlayLayerGpu&&) = delete;
  ~OverlayLayerGpu();

  // Uploads the cell textures of `handle` (set_overlays()). nullptr drops them. Returns false with `err` set when a
  // texture cannot be created; nothing is drawn then. A set whose data() is the one already uploaded only replaces
  // the configuration (WI-2.437); the caller keeps the previous set alive until this returns.
  bool upload(const OverlaySet* handle, std::string& err);

  // True while the arrays of a set are on the GPU (tests of the WI-2.437 re-upload skip).
  [[nodiscard]] const OverlaySet* uploaded_data() const { return uploaded_ ? set_ : nullptr; }

  // The arrow/stroke pass state (WI-2.434), for tests and the overlay benchmarks.
  [[nodiscard]] const ArrowPass& arrows() const { return arrows_; }

  // Draws passes 2-10 and 12 into a w x h layer for `view`, limited to `picture` (device-px rect of the drawn picture),
  // and fills `out` for pl_frame.overlays. Returns false when there is nothing to draw (no set, no enabled layer with
  // data) or on error (`err` set). The id buffer is drawn first, whether or not any layer is visible (WI-2.435).
  bool draw(const ViewState& view, int w, int h, float dpr, const pl_rect2df& picture, struct pl_overlay& out,
            std::string& err);

  // After the frame: reads back the id buffer if this frame drew it, otherwise forgets the previous ids.
  void end_frame();
  // The id under device pixel (px, py) from the newest completed readback (kIdNone when none; §10.5.7).
  uint32_t lookup_id(int px, int py) { return ids_.lookup(px, py); }
  IdBuffer& ids() { return ids_; }

 private:
  bool draw_ids(const CellPassConstants& pc, int w, int h, std::string& err);
  bool ensure_static(std::string& err);
  bool ensure_layer(int w, int h, std::string& err);
  bool update_lines(const ViewState& view, int w, int h, bool& lines, std::string& err);  // WI-2.432
  bool run_lines(const CellPassConstants& pc, std::string& err);
  [[nodiscard]] bool deblock_visible(const CellPassConstants& pc) const;  // WI-2.436
  MarkView update_marks(const CellPassConstants& pc);                     // WI-2.436
  bool draw_upper(const CellPassConstants& pc, bool deblock, bool arrows, const MarkView& mv, std::string& err);
  void drop_set();

  pl_gpu gpu_;
  pl_tex layer_ = nullptr;
  pl_tex palette_ = nullptr;
  pl_tex halo_ = nullptr;         // kOverlayHalo, one texel per §10.5.2 row (WI-2.436)
  std::array<pl_tex, 5> luts_{};  // Lut enum order: Viridis, Turbo, Cividis (= viridis, #471), Jet, Hot
  pl_tex attr_ = nullptr;
  pl_tex heat_ = nullptr;
  pl_tex heat_valid_ = nullptr;
  pl_tex deblock_ = nullptr;  // RG8UI OverlaySet::deblock_edges (WI-2.436)
  OverlayConfig cfg_{};
  uint32_t cells_w_ = 0;
  uint32_t cells_h_ = 0;
  CellPasses passes_;
  const OverlaySet* set_ = nullptr;  // data() of the uploaded set (owned by the surface); grid lines are built from it
  bool uploaded_ = false;            // set_'s arrays are fully uploaded
  GridLineCache grid_cache_;         // WI-2.432: culled line list and its rebuild triggers
  LinePass lines_;
  ArrowPass arrows_;
  MarkPass marks_;  // WI-2.436: selection outline, probe crosshair, selection rectangle
  std::vector<MarkRect> mark_rects_;
  IdBuffer ids_;
  pl_tex block_ids_ = nullptr;  // R32UI CellGrid.block_id for the id-only fill pass
  bool ids_drawn_ = false;      // this frame's id passes ran
  struct pl_overlay_part part_ = {};
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_OVERLAY_LAYER_H
