// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay passes 8 (intra direction strokes) and 9 (MV arrows) of chapter 10 §10.5.6 (WI-2.434 steps 2, 3 and 6):
// vertex pulling from an instance texture pair (§10.5, §10.5.1), a static integer corner buffer grown on demand, and
// the R-8K-1 viewport-culled upload. The GLSL is render/src/shaders/arrows.glsl. Internal to libovt-render.

#ifndef OVT_RENDER_SRC_OVERLAY_PASS_ARROWS_H
#define OVT_RENDER_SRC_OVERLAY_PASS_ARROWS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <libplacebo/gpu.h>

#include "overlay/arrow_instances.h"

namespace ovt::render::detail {

// Push constants of the arrow pass (GLSL std430 block `pc` of the prelude in pass_arrows.cpp).
struct ArrowPassConstants {
  std::array<float, 2> viewport_half{};  // drawable size / 2, device px
  std::array<float, 2> pan{};            // ViewState pan, luma samples
  std::array<float, 4> clip{};           // device-px rect of the drawn picture (x0, y0, x1, y1)
  float zoom = 1.0F;                     // device px per luma sample
  float dpr = 1.0F;                      // device pixel ratio: arrow sizes, dot size and dash scale with it
  float min_px = 2.0F;                   // OverlayConfig.mv_min_px
  uint32_t mode = 0;                     // kModeBiDraw / kModeSkipBi of arrows.glsl
  int32_t first = 0;                     // first instance of the draw
  int32_t tex_w = 0;                     // instance texture width (texels per row)
};
static_assert(sizeof(ArrowPassConstants) == 56, "must match the std430 layout of the arrow prelude");

// Instance texture width; rows = ceil(instances / width).
inline constexpr int kInstanceTexWidth = 4096;

class ArrowPass {
 public:
  ArrowPass() = default;
  ArrowPass(const ArrowPass&) = delete;
  ArrowPass& operator=(const ArrowPass&) = delete;
  ArrowPass(ArrowPass&&) = delete;
  ArrowPass& operator=(ArrowPass&&) = delete;
  ~ArrowPass() = default;  // destroy() must have run (the objects belong to a pl_gpu)

  // Keeps `full` (owned by the OverlaySet, which outlives the next set()) and uploads it, unless it has more than
  // `culled_threshold` instances (R-8K-1), in which case draw() uploads only the instances near the viewport.
  // nullptr drops everything.
  bool set(pl_gpu gpu, const ArrowInstances* full, uint64_t culled_threshold, std::string& err);

  // Pass 8 (IntraDirs) and pass 9 (MvL1, MvL0, then MvBi) for `layers`, blended over `target`. `pc` carries the view;
  // first/mode/tex_w are set here. Nothing is drawn below R-MV-1 (cell < 4 device px). Returns false on error.
  bool draw_intra(pl_gpu gpu, pl_tex target, pl_tex palette, uint32_t layers, const ArrowPassConstants& pc,
                  std::string& err);
  bool draw_mvs(pl_gpu gpu, pl_tex target, pl_tex palette, uint32_t layers, const ArrowPassConstants& pc,
                std::string& err);

  // True when there is anything to draw for `layers`.
  [[nodiscard]] bool has_work(uint32_t layers) const;

  // Instances resident on the GPU (the culled subset on the R-8K-1 path) and their texture bytes.
  [[nodiscard]] size_t resident_instances() const { return resident_seg_.end; }
  [[nodiscard]] size_t texture_bytes() const;

  void destroy(pl_gpu gpu);

 private:
  bool upload(pl_gpu gpu, const ArrowInstances& inst, std::string& err);
  bool ensure_view(pl_gpu gpu, const ArrowPassConstants& pc, std::string& err);
  bool ensure_pass(pl_gpu gpu, pl_fmt target_format, std::string& err);
  bool ensure_corners(pl_gpu gpu, size_t vertices, std::string& err);
  bool run(pl_gpu gpu, pl_tex target, pl_tex palette, ArrowPassConstants pc, uint32_t first, uint32_t end,
           uint32_t mode, std::string& err);
  void drop_textures(pl_gpu gpu);

  const ArrowInstances* full_ = nullptr;
  bool culled_ = false;
  std::array<float, 4> built_rect_{};  // image rect (25 % margin) the culled subset was built for
  ArrowSegments resident_seg_;         // segments of what the textures hold
  pl_tex geom_ = nullptr;
  pl_tex attr_ = nullptr;
  pl_buf corners_ = nullptr;
  size_t corner_capacity_ = 0;   // vertices
  pl_fmt corner_fmt_ = nullptr;  // r8u, or wider where the vertex stride must be aligned
  pl_pass pass_ = nullptr;
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_PASS_ARROWS_H
