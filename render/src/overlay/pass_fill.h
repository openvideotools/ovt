// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The per-cell overlay passes 2-4 of chapter 10 §10.5.6 (heatmap, prediction / reference-index fill, skip hatch;
// WI-2.431): one full-viewport quad each, written with libplacebo's raw pass API. The fragment shaders live in
// render/src/shaders/{heatmap,fill,hatch}.glsl; this file adds the prelude they share. Internal to libovt-render.

#ifndef OVT_RENDER_SRC_OVERLAY_PASS_FILL_H
#define OVT_RENDER_SRC_OVERLAY_PASS_FILL_H

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>

#include <libplacebo/gpu.h>

namespace ovt::render::detail {

// Push constants of the cell passes (GLSL std430 block `pc`; the order and offsets are those of the prelude).
struct CellPassConstants {
  std::array<float, 2> viewport_half{};  // drawable size / 2, device px
  std::array<float, 2> pan{};            // ViewState pan, luma samples
  std::array<float, 4> clip{};           // device-px rect of the drawn picture (x0, y0, x1, y1); nothing outside it
  float zoom = 1.0F;                     // device px per luma sample
  float alpha = 1.0F;                    // heatmap alpha (OverlayConfig.heatmap_alpha)
  float dpr = 1.0F;                      // device pixel ratio
  uint32_t layers = 0;                   // OverlayConfig.layers
  std::array<int32_t, 2> cells{};        // cells_w, cells_h
};
static_assert(sizeof(CellPassConstants) == 56, "must match the std430 layout of the shader prelude");

enum class CellPass : uint8_t { Heatmap, Fill, Hatch };

class CellPasses {
 public:
  CellPasses() = default;
  CellPasses(const CellPasses&) = delete;
  CellPasses& operator=(const CellPasses&) = delete;
  CellPasses(CellPasses&&) = delete;
  CellPasses& operator=(CellPasses&&) = delete;
  ~CellPasses() = default;  // destroy() must have run (the passes belong to a pl_gpu)

  // Blends one pass over all of `target` (straight-alpha output, "over" into a premultiplied layer). `textures` are
  // the pass's samplers in prelude order: Heatmap {heat, heat_valid, lut}; Fill and Hatch {cells, palette}. The pass
  // is compiled on first use. Returns false with `err` set when the pass cannot be created.
  bool run(pl_gpu gpu, CellPass pass, pl_tex target, const CellPassConstants& constants,
           std::initializer_list<pl_tex> textures, std::string& err);

  void destroy(pl_gpu gpu);

 private:
  std::array<pl_pass, 3> passes_{};
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_PASS_FILL_H
