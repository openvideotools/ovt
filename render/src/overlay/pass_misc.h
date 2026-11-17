// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay passes 10 and 12 of chapter 10 §10.5.6 (WI-2.436): the selection outline and the pixel-probe crosshair and
// selection rectangle. They are draw-time uniforms of OverlayConfig, so the geometry (a few axis-aligned device-pixel
// rectangles, "≤ 64 vertices immediate" in §10.5.1) is rebuilt on the CPU every render and drawn from host memory.
// Pass 7 (deblocking edges) is a cell pass: render/src/shaders/deblock.glsl, run by CellPasses. Internal to
// libovt-render.

#ifndef OVT_RENDER_SRC_OVERLAY_PASS_MISC_H
#define OVT_RENDER_SRC_OVERLAY_PASS_MISC_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/render_api.h"

namespace ovt::render::detail {

// §10.5.1: deblocking edges are drawn only when a 4x4 cell is at least this many device px.
inline constexpr float kDeblockMinCellPx = 8.0F;

// One solid device-pixel rectangle [x0, x1) x [y0, y1) in straight-alpha RGBA (0xRRGGBBAA).
struct MarkRect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // NOLINT(readability-isolate-declaration)
  uint32_t rgba = 0;
};

// The mapping of the drawn picture: device = (image - pan) * zoom + viewport_half (§10.2.3), limited to `clip`.
struct MarkView {
  std::array<float, 2> viewport_half{};
  std::array<float, 2> pan{};
  std::array<float, 4> clip{};  // x0, y0, x1, y1 device px
  float zoom = 1.0F;
  float dpr = 1.0F;
};

// The rectangles of passes 10 and 12 for `cfg`, in draw order (each halo before its line):
//   Selection (cfg.selected_block, cfg.selected_mv): the rect of that Block / MotionVector, outlined by a band of
//     2 x round(dpr) device px of row 15 centred on its edges, with a round(dpr)-px #000000 halo outside (§10.5.2).
//   Probe (cfg.probe_x/y): a round(dpr)-px row-17 horizontal and vertical line through the sample centre across the
//     drawn picture, each with its halo on both sides.
//   SelectionRect (cfg.selection_rect): row-18 fill (alpha 0x33) inside a round(dpr)-px row-18 border, halo outside.
// Nothing is emitted for a layer bit that is off or a value that is "none" (0xFFFFFFFF, -1, zero w/h, out of range).
OVT_RENDER_API void build_marks(const OverlaySet& set, const OverlayConfig& cfg, const MarkView& view,
                                std::vector<MarkRect>& out);

class MarkPass {
 public:
  // Blends `rects` over `target` (straight alpha "over", like the cell passes), clipped to view.clip.
  bool run(pl_gpu gpu, pl_tex target, const std::vector<MarkRect>& rects, const MarkView& view, std::string& err);
  void destroy(pl_gpu gpu);

 private:
  pl_pass pass_ = nullptr;
  std::vector<float> vertices_;
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_PASS_MISC_H
