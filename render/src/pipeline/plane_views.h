// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Plane modes Y / U / V / gray and raw code values (chapter 10 §10.6.1, WI-1.408). Internal to libovt-render.

#ifndef OVT_RENDER_SRC_PIPELINE_PLANE_VIEWS_H
#define OVT_RENDER_SRC_PIPELINE_PLANE_VIEWS_H

#include <array>
#include <string>

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"
#include "pipeline/frame_upload.h"

namespace ovt::render::detail {

// ViewState.plane_mode values (§10.2.3).
enum class PlaneMode : int { Yuv = 0, Y = 1, U = 2, V = 3, Gray = 4 };

// The plane mode a ViewState asks for; values outside 0..4 fall back to Yuv.
OVT_RENDER_API PlaneMode plane_mode(const ViewState& view);

// Neutral (achromatic) chroma code of a bit depth: 128 << (bit_depth - 8), and 1 << (bit_depth - 1) below 8 bits
// (FR-PLN-001).
OVT_RENDER_API unsigned neutral_code(unsigned bit_depth);

// Source rectangle of plane `plane` (0 luma, 1/2 chroma) for a luma-space rectangle: the chroma plane's subsampling
// divides the coordinates (§10.6.1 "crop scaled by sub_x/sub_y").
OVT_RENDER_API pl_rect2df plane_rect(const pl_rect2df& luma_rect, int sub_x, int sub_y);

// Builds the secondary pl_frames of the plane modes and adjusts the render parameters and the target for them. One
// instance per surface: it owns the 1x1 neutral chroma textures of gray mode and the replicate hooks; the plane
// textures themselves stay in the FrameSlot, so switching modes never re-uploads (§10.11 WI-1.408 notes).
class PlaneViews {
 public:
  PlaneViews();
  PlaneViews(const PlaneViews&) = delete;
  PlaneViews& operator=(const PlaneViews&) = delete;
  PlaneViews(PlaneViews&&) = delete;
  PlaneViews& operator=(PlaneViews&&) = delete;
  ~PlaneViews() = default;  // destroy(gpu) frees the textures; the owner calls it before the pl_gpu goes

  // Fills `image` (including image.crop) for `view` from `slot`, given the luma-space source rectangle. Mode Yuv
  // copies slot.frame unchanged. Returns "" or an RND-0002 message when a neutral texture cannot be created.
  std::string build_image(pl_gpu gpu, const FrameSlot& slot, const ViewState& view, const pl_rect2df& luma_rect,
                          struct pl_frame& image);

  // Applies the plane-mode parts of §10.6.1 on top of the RenderParamSet output: the replicate hook, nearest
  // upscaling for chroma planes, and for raw_code_values the pass-through target (sRGB / BT.709 on both sides, no
  // dithering). `params` is a copy of RenderParamSet::params(); it keeps pointing at that set's members.
  void apply(const ViewState& view, const struct pl_frame& image, struct pl_render_params& params,
             struct pl_frame& target) const;

  void destroy(pl_gpu gpu);

 private:
  pl_tex neutral_tex(pl_gpu gpu, int sample_bytes, unsigned bit_depth, std::string& err);

  // Neutral 1x1 textures for r8 (index 0) and r16 (index 1), and the code each one holds.
  std::array<pl_tex, 2> neutral_{};
  std::array<unsigned, 2> neutral_value_{};
  struct pl_hook replicate_hook_ = {};
  std::array<const struct pl_hook*, 1> hooks_{};  // pl_render_params.hooks
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PIPELINE_PLANE_VIEWS_H
