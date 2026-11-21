// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay layer and pl_overlay wiring (chapter 10 §10.5, §10.5.1, §10.5.6; WI-2.431 steps 1-3 and 5).

#include "overlay/overlay_layer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "luts/hot.h"
#include "luts/jet.h"
#include "luts/turbo.h"
#include "luts/viridis.h"
#include "ovt/render/overlay_colors.h"

namespace ovt::render::detail {

namespace {

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

constexpr uint32_t kFillLayers = bit(OverlayLayer::PredFill) | bit(OverlayLayer::RefIdxFill);
constexpr uint32_t kLineLayers = bit(OverlayLayer::GridTU) | bit(OverlayLayer::GridPU) | bit(OverlayLayer::GridCU) |
                                 bit(OverlayLayer::GridCTU) | bit(OverlayLayer::GridPartition);

pl_tex make_texture(pl_gpu gpu, const char* fmt_name, int w, int h, const void* data, size_t row_pitch,
                    std::string& err) {
  pl_fmt fmt = pl_find_named_fmt(gpu, fmt_name);
  if (fmt == nullptr || (fmt->caps & PL_FMT_CAP_SAMPLEABLE) == 0) {
    err = std::string("overlay texture format ") + fmt_name + " is not sampleable";
    return nullptr;
  }
  struct pl_tex_params params = {};
  params.w = w;
  params.h = h;
  params.format = fmt;
  params.sampleable = true;
  params.host_writable = true;
  pl_tex tex = pl_tex_create(gpu, &params);
  if (tex == nullptr) {
    err = std::string("pl_tex_create failed for an overlay ") + fmt_name + " texture";
    return nullptr;
  }
  struct pl_tex_transfer_params tp = {};
  tp.tex = tex;
  tp.row_pitch = row_pitch;
  tp.ptr = const_cast<void*>(data);  // NOLINT(cppcoreguidelines-pro-type-const-cast): upload only reads it
  if (!pl_tex_upload(gpu, &tp)) {
    pl_tex_destroy(gpu, &tex);
    err = std::string("pl_tex_upload failed for an overlay ") + fmt_name + " texture";
    return nullptr;
  }
  return tex;
}

// Creates `tex` from `data` when it holds `cells` texels of `texel` bytes (cells_w per row). True when the column is
// absent (nothing to create) or the texture was created.
bool cell_texture(pl_gpu gpu, pl_tex& tex, const char* fmt, int w, int h, const void* data, size_t bytes, size_t texel,
                  std::string& err) {
  const size_t cells = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (bytes != cells * texel) {
    return true;
  }
  tex = make_texture(gpu, fmt, w, h, data, static_cast<size_t>(w) * texel, err);
  return tex != nullptr;
}

}  // namespace

OverlayLayerGpu::~OverlayLayerGpu() {
  drop_set();
  arrows_.destroy(gpu_);
  marks_.destroy(gpu_);
  passes_.destroy(gpu_);
  lines_.destroy(gpu_);
  pl_tex_destroy(gpu_, &layer_);
  pl_tex_destroy(gpu_, &palette_);
  pl_tex_destroy(gpu_, &halo_);
  for (pl_tex& t : luts_) {
    pl_tex_destroy(gpu_, &t);
  }
}

void OverlayLayerGpu::drop_set() {
  pl_tex_destroy(gpu_, &attr_);
  pl_tex_destroy(gpu_, &heat_);
  pl_tex_destroy(gpu_, &heat_valid_);
  pl_tex_destroy(gpu_, &deblock_);
  pl_tex_destroy(gpu_, &block_ids_);
  std::string ignored;
  arrows_.set(gpu_, nullptr, 0, ignored);
  cells_w_ = 0;
  cells_h_ = 0;
  cfg_ = OverlayConfig{};
  set_ = nullptr;
  uploaded_ = false;
  grid_cache_.reset();
}

bool OverlayLayerGpu::ensure_static(std::string& err) {
  if (palette_ == nullptr) {
    // The fill colours of §10.5.2 (WI-2.439). The palette is always the standard one: OverlayConfig has no
    // colour-blind switch yet; the GUI work item for the K toggle adds it.
    std::array<uint8_t, kOverlayColors.size() * 4> bytes{};
    for (size_t i = 0; i < kOverlayColors.size(); ++i) {
      const uint32_t rgba = overlay_fill_rgba(i, false);
      for (size_t c = 0; c < 4; ++c) {
        bytes.at((i * 4) + c) = static_cast<uint8_t>(rgba >> (24U - (8U * c)));
      }
    }
    palette_ = make_texture(gpu_, "rgba8", static_cast<int>(kOverlayColors.size()), 1, bytes.data(), bytes.size(), err);
    if (palette_ == nullptr) {
      return false;
    }
  }
  if (halo_ == nullptr) {  // WI-2.436: the 1-px halo of each row (kOverlayHalo, issue #633)
    std::array<uint8_t, kOverlayHalo.size() * 4> bytes{};
    for (size_t i = 0; i < kOverlayHalo.size(); ++i) {
      for (size_t c = 0; c < 4; ++c) {
        bytes.at((i * 4) + c) = static_cast<uint8_t>(kOverlayHalo.at(i) >> (24U - (8U * c)));
      }
    }
    halo_ = make_texture(gpu_, "rgba8", static_cast<int>(kOverlayHalo.size()), 1, bytes.data(), bytes.size(), err);
    if (halo_ == nullptr) {
      return false;
    }
  }
  // SPEC-ASSUMPTION(#471): the cividis data's licence is not in the licence policy yet, so Lut::Cividis draws with
  // viridis (the other colour-blind-safe map) until the owner decides.
  const std::array<const std::array<uint8_t, 1024>*, 5> tables = {&luts::kViridis, &luts::kTurbo, &luts::kViridis,
                                                                  &luts::kJet, &luts::kHot};
  for (size_t i = 0; i < luts_.size(); ++i) {
    if (luts_.at(i) == nullptr) {
      luts_.at(i) = make_texture(gpu_, "rgba8", 256, 1, tables.at(i)->data(), tables.at(i)->size(), err);
      if (luts_.at(i) == nullptr) {
        return false;
      }
    }
  }
  return true;
}

bool OverlayLayerGpu::upload(const OverlaySet* handle, std::string& err) {
  // WI-2.437: a handle sharing the arrays already uploaded (the caller keeps the previous set alive meanwhile) changes
  // only uniforms: keep the textures, the arrow upload and the culled grid lines.
  if (handle != nullptr && uploaded_ && &handle->data() == set_) {
    cfg_ = handle->config;
    return true;
  }
  drop_set();
  if (handle == nullptr) {
    return true;
  }
  if (!ensure_static(err)) {
    return false;
  }
  // The grid lines need only the block tree and the partition lines, not the cell grid.
  const OverlaySet* set = &handle->data();
  cfg_ = handle->config;
  set_ = set;
  if (set->cells_w == 0 || set->cells_h == 0) {
    uploaded_ = true;
    return true;
  }
  const auto w = static_cast<int>(set->cells_w);
  const auto h = static_cast<int>(set->cells_h);
  const size_t cells = static_cast<size_t>(set->cells_w) * set->cells_h;
  // WI-2.435: block ids for the id-only fill pass; pass 3/4 attributes; WI-2.436: pass 7 edge classes.
  if (!cell_texture(gpu_, block_ids_, "r32u", w, h, set->cell_block_id.data(), set->cell_block_id.size() * 4, 4, err) ||
      !cell_texture(gpu_, attr_, "rgba8u", w, h, set->cell_attributes.data(), set->cell_attributes.size(), 4, err) ||
      !cell_texture(gpu_, deblock_, "rg8u", w, h, set->deblock_edges.data(), set->deblock_edges.size(), 2, err)) {
    return false;
  }
  if (set->heatmap.size() == cells && set->heat_valid.size() == cells) {
    heat_ = make_texture(gpu_, "r16", w, h, set->heatmap.data(), size_t{set->cells_w} * 2, err);
    std::vector<uint8_t> valid(cells);
    for (size_t i = 0; i < cells; ++i) {
      valid[i] = set->heat_valid[i] != 0 ? 0xFF : 0x00;
    }
    heat_valid_ = heat_ != nullptr ? make_texture(gpu_, "r8", w, h, valid.data(), size_t{set->cells_w}, err) : nullptr;
    if (heat_valid_ == nullptr) {
      drop_set();
      return false;
    }
  }
  // WI-2.434: arrow and stroke instances; above the R-8K-1 cell count only the viewport's share is uploaded (draw()).
  const bool culled = cells > kCulledBuildCells;
  if (!arrows_.set(gpu_, &set->arrows, culled ? 0 : std::numeric_limits<uint64_t>::max(), err)) {
    drop_set();
    return false;
  }
  cells_w_ = set->cells_w;
  cells_h_ = set->cells_h;
  uploaded_ = true;
  return true;
}

bool OverlayLayerGpu::ensure_layer(int w, int h, std::string& err) {
  pl_fmt fmt = pl_find_named_fmt(gpu_, "rgba8");
  constexpr auto kNeeded =
      static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_BLENDABLE);
  if (fmt == nullptr || (fmt->caps & kNeeded) != kNeeded) {
    err = "overlay layer format rgba8 is not renderable/blendable";
    return false;
  }
  struct pl_tex_params params = {};
  params.w = w;
  params.h = h;
  params.format = fmt;
  params.sampleable = true;
  params.renderable = true;
  params.blit_dst = true;  // pl_tex_clear_ex
  if (!pl_tex_recreate(gpu_, &layer_, &params)) {
    err = "pl_tex_recreate failed for the overlay layer";
    return false;
  }
  return true;
}

bool OverlayLayerGpu::update_lines(const ViewState& view, int w, int h, bool& lines, std::string& err) {
  lines = false;
  if ((cfg_.layers & kLineLayers) == 0 || set_ == nullptr) {
    return true;
  }
  // WI-2.432 step 5: rebuild only on a new set, a level threshold crossing or a pan beyond the pre-fetched margin.
  const ImageRect visible = visible_image_rect(w, h, view.pan_x, view.pan_y, view.zoom);
  if (grid_cache_.update(*set_, cfg_.layers, view.zoom, visible) && !lines_.upload(gpu_, grid_cache_.lines(), err)) {
    grid_cache_.reset();
    return false;
  }
  lines = !grid_cache_.lines().vertices.empty();
  return true;
}

// 5 grids TU -> PU -> CU -> CTU and 6 slice/tile boundaries, one draw in that order (§10.5.3).
bool OverlayLayerGpu::run_lines(const CellPassConstants& pc, std::string& err) {
  LinePassConstants lc;
  lc.viewport_half = pc.viewport_half;
  lc.pan = pc.pan;
  lc.clip = pc.clip;
  lc.picture = {static_cast<float>(set_->picture_w), static_cast<float>(set_->picture_h)};
  lc.zoom = pc.zoom;
  lc.line_scale = std::max(cfg_.line_px, 0.0F) * pc.dpr;
  return lines_.run(gpu_, layer_, palette_, lc, err);
}

namespace {

ArrowPassConstants arrow_constants(const CellPassConstants& pc, float min_px) {
  ArrowPassConstants apc;
  apc.viewport_half = pc.viewport_half;
  apc.pan = pc.pan;
  apc.clip = pc.clip;
  apc.zoom = pc.zoom;
  apc.dpr = pc.dpr;
  apc.min_px = min_px;
  return apc;
}

}  // namespace

// The id buffer of §10.5.6: the id-only run of pass 3 (block_id + 1, even when no fill layer is on), then pass 9's
// arrows (0x80000000 | mv_index) under the same layer bits and R-MV-1 rule as the visible arrows.
bool OverlayLayerGpu::draw_ids(const CellPassConstants& pc, int w, int h, std::string& err) {
  if (set_ == nullptr || (block_ids_ == nullptr && !arrows_.has_work(cfg_.layers))) {
    return true;
  }
  if (!ids_.begin(w, h, err)) {
    return false;
  }
  if (block_ids_ != nullptr && !passes_.run(gpu_, CellPass::BlockId, ids_.target(), pc, {block_ids_}, err)) {
    return false;
  }
  if (!arrows_.draw_mvs(gpu_, ids_.target(), palette_, cfg_.layers, arrow_constants(pc, cfg_.mv_min_px), err, true)) {
    return false;
  }
  ids_drawn_ = true;
  return true;
}

// Pass 7 runs only when a cell is at least 8 device px (§10.5.1).
bool OverlayLayerGpu::deblock_visible(const CellPassConstants& pc) const {
  return (cfg_.layers & bit(OverlayLayer::DeblockEdges)) != 0 && deblock_ != nullptr &&
         4.0F * pc.zoom >= kDeblockMinCellPx;
}

// Passes 10 and 12 are draw-time uniforms: their rectangles are rebuilt every render (§10.5.1).
MarkView OverlayLayerGpu::update_marks(const CellPassConstants& pc) {
  MarkView mv;
  mv.viewport_half = pc.viewport_half;
  mv.pan = pc.pan;
  mv.clip = pc.clip;
  mv.zoom = pc.zoom;
  mv.dpr = pc.dpr;
  mark_rects_.clear();
  if (set_ != nullptr) {
    build_marks(*set_, cfg_, mv, mark_rects_);
  }
  return mv;
}

// 7 deblocking edges, 8 intra direction strokes, 9 MV arrows (L1, L0, bi), 10 selection outline, 12 probe crosshair
// and selection rectangle (11, labels, is not drawn yet).
bool OverlayLayerGpu::draw_upper(const CellPassConstants& pc, bool deblock, bool arrows, const MarkView& mv,
                                 std::string& err) {
  if (deblock && !passes_.run(gpu_, CellPass::Deblock, layer_, pc, {deblock_, palette_, halo_}, err)) {
    return false;
  }
  if (arrows) {
    const ArrowPassConstants apc = arrow_constants(pc, cfg_.mv_min_px);
    if (!arrows_.draw_intra(gpu_, layer_, palette_, cfg_.layers, apc, err) ||
        !arrows_.draw_mvs(gpu_, layer_, palette_, cfg_.layers, apc, err)) {
      return false;
    }
  }
  return marks_.run(gpu_, layer_, mark_rects_, mv, err);
}

void OverlayLayerGpu::end_frame() {
  if (ids_drawn_) {
    ids_.readback();
  } else {
    ids_.invalidate();
  }
  ids_drawn_ = false;
}

bool OverlayLayerGpu::draw(const ViewState& view, int w, int h, float dpr, const pl_rect2df& picture,
                           struct pl_overlay& out, std::string& err) {
  const uint32_t layers = cfg_.layers;
  CellPassConstants pc;
  pc.viewport_half = {static_cast<float>(w) / 2.0F, static_cast<float>(h) / 2.0F};
  pc.pan = {static_cast<float>(view.pan_x), static_cast<float>(view.pan_y)};
  // pl_rect2df may be flipped (x1 < x0); the shader wants the normalised rect.
  pc.clip = {std::min(picture.x0, picture.x1), std::min(picture.y0, picture.y1), std::max(picture.x0, picture.x1),
             std::max(picture.y0, picture.y1)};
  pc.zoom = static_cast<float>(view.zoom);
  pc.alpha = cfg_.heatmap_alpha;
  pc.dpr = dpr > 0.0F ? dpr : 1.0F;
  pc.layers = layers;
  pc.cells = {static_cast<int32_t>(cells_w_), static_cast<int32_t>(cells_h_)};
  if (!draw_ids(pc, w, h, err)) {
    return false;
  }

  const bool heatmap = (layers & bit(OverlayLayer::Heatmap)) != 0 && heat_ != nullptr;
  const bool fill = (layers & kFillLayers) != 0 && attr_ != nullptr;
  const bool hatch = (layers & bit(OverlayLayer::SkipHatch)) != 0 && attr_ != nullptr;
  bool lines = false;
  if (!update_lines(view, w, h, lines, err)) {
    return false;
  }
  const bool arrows = arrows_.has_work(layers);
  const bool deblock = deblock_visible(pc);
  const MarkView mv = update_marks(pc);
  if (!heatmap && !fill && !hatch && !lines && !deblock && !arrows && mark_rects_.empty()) {
    return false;
  }
  if (!ensure_layer(w, h, err)) {
    return false;
  }
  pl_tex_clear_ex(gpu_, layer_, pl_clear_color{});  // transparent

  // §10.5.6 draw order: 2 heatmap, 3 fills, 4 skip hatch.
  if (heatmap) {
    const auto lut = static_cast<size_t>(cfg_.lut) < luts_.size() ? static_cast<size_t>(cfg_.lut) : 0;
    if (!passes_.run(gpu_, CellPass::Heatmap, layer_, pc, {heat_, heat_valid_, luts_.at(lut)}, err)) {
      return false;
    }
  }
  if (fill && !passes_.run(gpu_, CellPass::Fill, layer_, pc, {attr_, palette_}, err)) {
    return false;
  }
  if (hatch && !passes_.run(gpu_, CellPass::Hatch, layer_, pc, {attr_, palette_}, err)) {
    return false;
  }
  if (lines && !run_lines(pc, err)) {
    return false;
  }
  if (!draw_upper(pc, deblock, arrows, mv, err)) {
    return false;
  }

  // §10.5: sRGB BT.709, full range, premultiplied (the passes blend "over"), one full-size part in target pixels.
  // SPEC-ASSUMPTION(#467): §10.5 says PL_ALPHA_PREMULTIPLIED, the WI-2.431 note says straight alpha; the layer the
  // passes leave is premultiplied, so §10.5 holds.
  // libplacebo blends it in linear light and maps its white to SDR reference white (203 nits) on HDR targets.
  part_ = {};
  part_.src = {0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h)};
  part_.dst = part_.src;
  out = {};
  out.tex = layer_;
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

}  // namespace ovt::render::detail
