// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The grid line pass of chapter 10 §10.5.3 (WI-2.432): passes 5 (partition grids TU -> PU -> CU -> CTU) and 6 (slice /
// tile boundaries) of §10.5.6. The CPU side walks the block tree with rect pruning and R-GRID-1 level culling and
// emits each edge once (left/top rule); the GPU side expands every line to a pixel-snapped screen-aligned quad by
// vertex pulling of its two endpoints (K = 6). Internal to libovt-render.

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
  float x = 0;
  float y = 0;
  uint8_t color_idx = 0;
  std::array<uint8_t, 3> pad{};
};
static_assert(sizeof(GridVertex) == 12, "§10.5.1 line-list vertex is 12 bytes");

// §10.5.2 colour rows: CTU 8, CU 9, PU 10, TU 11, slice/tile boundary 12.
inline constexpr uint8_t kColorGridCtu = 8;
inline constexpr uint8_t kColorPartition = 12;

// R-GRID-1: a level whose smallest block is below this many device px is not drawn.
inline constexpr float kGridMinPx = 6.0F;
// §10.5.1: the line list covers the visible image rect expanded by this fraction of its size on each side.
inline constexpr float kGridMargin = 0.25F;

// An axis-aligned rect in luma samples, x0 <= x1, y0 <= y1.
struct ImageRect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // NOLINT(readability-isolate-declaration)
};

// The visible image rect of a w x h drawable centred on (pan_x, pan_y) at `zoom` device px per luma sample.
OVT_RENDER_API ImageRect visible_image_rect(int w, int h, double pan_x, double pan_y, double zoom);

// `r` grown by kGridMargin of its width / height on every side.
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
// changes) or a visible rect that leaves the area pre-fetched with the 25 % margin.
class OVT_RENDER_API GridLineCache {
 public:
  // Returns true when the line list was rebuilt.
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
  // destroy() must run before the pl_gpu goes away; the object holds plain libplacebo handles.
  // Uploads the endpoints of `lines` into the RGBA32F endpoint texture (one texel per vertex: x, y, colour row).
  bool upload(pl_gpu gpu, const GridLines& lines, std::string& err);

  // Draws the uploaded lines over `target` with the §10.5.2 `palette`. No-op without lines.
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
