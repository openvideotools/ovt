// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Grid line pass (§10.5.3, WI-2.432), passes 5-6 of §10.5.6: block-tree walk with rect pruning, R-GRID-1 level culling
// and the left/top edge rule on the CPU; pixel-snapped quads by vertex pulling of both endpoints (K = 6) on the GPU.

#ifndef OVT_RENDER_SRC_OVERLAY_PASS_LINES_H
#define OVT_RENDER_SRC_OVERLAY_PASS_LINES_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

#include "overlay/overlay_set_impl.h"
#include "ovt/render/render_api.h"

namespace ovt::render::detail {

// One line-list vertex of §10.5.1 (12 B): an endpoint in luma samples and the §10.5.2 colour row of its line.
struct GridVertex {
  float x = 0, y = 0;  // NOLINT(readability-isolate-declaration)
  uint8_t color_idx = 0;
  std::array<uint8_t, 3> pad{};
};
static_assert(sizeof(GridVertex) == 12, "§10.5.1 line-list vertex is 12 bytes");

// §10.5.2 colour rows: CTU 8, CU 9, PU 10, TU 11, slice/tile boundary 12.
inline constexpr uint8_t kColorGridCtu = 8;
inline constexpr uint8_t kColorPartition = 12;

inline constexpr float kGridMinPx = 6.0F;    // R-GRID-1: levels with smaller blocks are not drawn
inline constexpr float kGridMargin = 0.25F;  // §10.5.1: pre-fetch margin per side, fraction of the visible size

struct ImageRect {                       // luma samples, x0 <= x1, y0 <= y1
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // NOLINT(readability-isolate-declaration)
};

// The visible image rect of a w x h drawable centred on (pan_x, pan_y) at `zoom`; expand_margin() adds kGridMargin.
OVT_RENDER_API ImageRect visible_image_rect(int w, int h, double pan_x, double pan_y, double zoom);
OVT_RENDER_API ImageRect expand_margin(const ImageRect& r);

// Bit i set = grid level slot i (GridLevel order TU, PU, CU, CTU) is drawn: its layer bit is in `layers` and its
// smallest block side times `zoom` is at least kGridMinPx (R-GRID-1).
OVT_RENDER_API uint32_t grid_levels_drawn(const OverlaySet& set, uint32_t layers, double zoom);

struct GridLines {
  std::vector<GridVertex> vertices;          // §10.5.6 order: TU, PU, CU, CTU, then slice/tile boundaries; 2 per line
  std::array<uint32_t, 5> level_vertices{};  // vertices per slot: TU, PU, CU, CTU, partition
  uint32_t levels = 0;                       // grid_levels_drawn() the list was built for
  ImageRect area;                            // blocks were kept when their rect intersects this rect
};

// WI-2.432 steps 1-2: walks set.grid_nodes from the root, prunes subtrees whose rect misses `area`, and emits for each
// block of a drawn level its top and left edges, plus its right / bottom edge when it touches the picture's right /
// bottom boundary, so every edge is emitted once. Partition lines (`with_partitions`) are emitted whole.
OVT_RENDER_API void build_grid_lines(const OverlaySet& set, uint32_t levels, bool with_partitions,
                                     const ImageRect& area, GridLines& out);

// Rebuild triggers of §10.5.1 (WI-2.432 step 5): a new set, a zoom crossing a level threshold (the drawn level mask
// changes) or a visible rect that leaves the pre-fetched area. update() returns true when it rebuilt the list.
class OVT_RENDER_API GridLineCache {
 public:
  bool update(const OverlaySet& set, uint32_t layers, double zoom, const ImageRect& visible);
  void reset() { valid_ = false; }
  [[nodiscard]] const GridLines& lines() const { return lines_; }
  [[nodiscard]] uint64_t builds() const { return builds_; }

 private:
  GridLines lines_;
  const OverlaySet* set_ = nullptr;
  uint32_t layers_ = 0;
  bool valid_ = false;
  uint64_t builds_ = 0;
};

// Push constants of the line pass (GLSL std430 block `pc` of lines.glsl).
struct LinePassConstants {
  std::array<float, 2> viewport_half{};  // drawable size / 2, device px
  std::array<float, 2> pan{};            // ViewState pan, luma samples
  std::array<float, 4> clip{};           // device-px rect of the drawn picture (x0, y0, x1, y1)
  std::array<float, 2> picture{};        // coded picture size, luma samples
  float zoom = 1.0F;                     // device px per luma sample
  float line_scale = 1.0F;               // OverlayConfig.line_px x device pixel ratio
  int32_t tex_w = 1;                     // width of the endpoint texture, texels
  int32_t pad = 0;
};
static_assert(sizeof(LinePassConstants) == 56, "must match the std430 layout of lines.glsl");

class LinePass {
 public:
  // upload(): endpoints into the RGBA32F texture (texel per vertex: x, y, colour row). run(): draws them over `target`
  // with the §10.5.2 `palette`, a no-op without lines. destroy() must run before the pl_gpu goes away.
  bool upload(pl_gpu gpu, const GridLines& lines, std::string& err);
  bool run(pl_gpu gpu, pl_tex target, pl_tex palette, LinePassConstants constants, std::string& err);
  void destroy(pl_gpu gpu);

 private:
  bool ensure_corners(pl_gpu gpu, int vertex_count, std::string& err);

  pl_pass pass_ = nullptr;
  pl_tex ends_ = nullptr;
  pl_buf corners_ = nullptr;
  pl_fmt corner_fmt_ = nullptr;
  int corner_vertices_ = 0;
  int lines_ = 0;
  int tex_w_ = 1;
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_PASS_LINES_H
