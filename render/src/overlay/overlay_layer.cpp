// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Overlay layer and pl_overlay wiring (chapter 10 §10.5, §10.5.1, §10.5.6; WI-2.431 steps 1-3 and 5).

#include "overlay/overlay_layer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "luts/hot.h"
#include "luts/jet.h"
#include "luts/turbo.h"
#include "luts/viridis.h"

namespace ovt::render::detail {

// clang-format off
const std::array<uint32_t, 33> kFillPalette = {
    0x2ECC4066U, 0xFF851B4DU, 0xB10DC94DU, 0x00E5FF4DU, 0xAAAAAA99U, 0x20C99766U, 0xFFDC0066U, 0xF012BE66U,  //  0- 7
    0xFFFFFFE6U, 0xFFFF00CCU, 0x00FFFFB3U, 0xFF00FF99U, 0xFF4136FFU, 0x7FDBFFCCU, 0x0074D9FFU, 0xFFFFFFFFU,  //  8-15
    0xFFD700FFU, 0xFF4136FFU, 0x39CCCC33U, 0xFFFFFFFFU, 0x2ECC40FFU, 0xFFDC00FFU,                            // 16-21
    0xE6194B66U, 0x3CB44B66U, 0xFFE11966U, 0x4363D866U, 0xF5823166U, 0x911EB466U, 0x46F0F066U, 0xF032E666U,  // 22-29
    0xE53935FFU, 0x1E88E5FFU, 0x43A047FFU,                                                                   // 30-32
};
// clang-format on

namespace {

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

constexpr uint32_t kFillLayers = bit(OverlayLayer::PredFill) | bit(OverlayLayer::RefIdxFill);

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

}  // namespace

OverlayLayerGpu::~OverlayLayerGpu() {
  drop_set();
  passes_.destroy(gpu_);
  pl_tex_destroy(gpu_, &layer_);
  pl_tex_destroy(gpu_, &palette_);
  for (pl_tex& t : luts_) {
    pl_tex_destroy(gpu_, &t);
  }
}

void OverlayLayerGpu::drop_set() {
  pl_tex_destroy(gpu_, &attr_);
  pl_tex_destroy(gpu_, &heat_);
  pl_tex_destroy(gpu_, &heat_valid_);
  cells_w_ = 0;
  cells_h_ = 0;
  cfg_ = OverlayConfig{};
}

bool OverlayLayerGpu::ensure_static(std::string& err) {
  if (palette_ == nullptr) {
    std::array<uint8_t, kFillPalette.size() * 4> bytes{};
    for (size_t i = 0; i < kFillPalette.size(); ++i) {
      for (size_t c = 0; c < 4; ++c) {
        bytes.at((i * 4) + c) = static_cast<uint8_t>(kFillPalette.at(i) >> (24U - (8U * c)));
      }
    }
    palette_ = make_texture(gpu_, "rgba8", static_cast<int>(kFillPalette.size()), 1, bytes.data(), bytes.size(), err);
    if (palette_ == nullptr) {
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

bool OverlayLayerGpu::upload(const OverlaySet* set, std::string& err) {
  drop_set();
  if (set == nullptr || set->cells_w == 0 || set->cells_h == 0) {
    return true;
  }
  if (!ensure_static(err)) {
    return false;
  }
  const auto w = static_cast<int>(set->cells_w);
  const auto h = static_cast<int>(set->cells_h);
  const size_t cells = static_cast<size_t>(set->cells_w) * set->cells_h;
  if (set->cell_attributes.size() == cells * 4) {
    attr_ = make_texture(gpu_, "rgba8u", w, h, set->cell_attributes.data(), size_t{set->cells_w} * 4, err);
    if (attr_ == nullptr) {
      return false;
    }
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
  cfg_ = set->config;
  cells_w_ = set->cells_w;
  cells_h_ = set->cells_h;
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

bool OverlayLayerGpu::draw(const ViewState& view, int w, int h, float dpr, const pl_rect2df& picture,
                           struct pl_overlay& out, std::string& err) {
  const uint32_t layers = cfg_.layers;
  const bool heatmap = (layers & bit(OverlayLayer::Heatmap)) != 0 && heat_ != nullptr;
  const bool fill = (layers & kFillLayers) != 0 && attr_ != nullptr;
  const bool hatch = (layers & bit(OverlayLayer::SkipHatch)) != 0 && attr_ != nullptr;
  if (!heatmap && !fill && !hatch) {
    return false;
  }
  if (!ensure_layer(w, h, err)) {
    return false;
  }
  pl_tex_clear_ex(gpu_, layer_, pl_clear_color{});  // transparent

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
