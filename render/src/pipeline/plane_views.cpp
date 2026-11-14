// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Plane modes Y / U / V / gray and raw code values (chapter 10 §10.6.1, WI-1.408).
//
// Modes 1-3 draw one stored plane as a grey image: a secondary pl_frame whose single plane is the slot's Y, Cb or Cr
// texture (at that plane's own resolution), declared as RGB and BT.709 / sRGB so libplacebo never tone-maps it (plane
// views are SDR views of code values, also for HDR content), with a PL_HOOK_RGB_INPUT hook that replicates the sampled
// channel to R, G and B. Mode 4 is the normal path with both chroma planes pointing at a 1x1 texture of the neutral
// chroma code. No mode uploads anything: the plane textures stay in the FrameSlot.

#include "pipeline/plane_views.h"

#include <cstdint>
#include <string>

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/sampling.h>

namespace ovt::render::detail {
namespace {

// PL_HOOK_RGB_INPUT on the plane texture: sample it 1:1 and replicate the stored channel (color.r) to R, G and B. For
// the chroma planes this is §10.6.1's value remap g = (c - 0.5) + 0.5, the identity on normalised full-range chroma,
// so the neutral code lands on mid-grey. The hook takes the texture rather than libplacebo's pre-sampled colour
// because the input stages are resizable: a returned shader must fix its own output size, which only the texture
// tells.
struct pl_hook_res replicate_red(void* /*priv*/, const struct pl_hook_params* params) {
  struct pl_hook_res res = {};
  if (params->tex == nullptr) {
    res.failed = true;
    return res;
  }
  pl_shader sh = pl_dispatch_begin(params->dispatch);
  // The hook's shader replaces libplacebo's own plane sampling, which would scale the samples to the declared bit
  // depth (an LSB-aligned 10-bit code in an R16 texture); scale here and hand back the normalised representation.
  res.repr = params->repr;
  struct pl_sample_src src = {};
  src.tex = params->tex;
  src.scale = pl_color_repr_normalize(&res.repr);
  struct pl_custom_shader body = {};
  body.input = PL_SHADER_SIG_COLOR;
  body.output = PL_SHADER_SIG_COLOR;
  body.body = "color.rgb = vec3(color.r);";
  body.output_w = params->tex->params.w;
  body.output_h = params->tex->params.h;
  if (!pl_shader_sample_direct(sh, &src) || !pl_shader_custom(sh, &body)) {
    pl_dispatch_abort(params->dispatch, &sh);
    res.failed = true;
    return res;
  }
  res.output = PL_HOOK_SIG_COLOR;
  res.sh = sh;
  res.color = params->color;
  res.components = 3;
  res.rect = params->rect;
  return res;
}

// Subsampling of plane `i` against the luma plane, from the texture sizes (2 for a half-size plane, else 1).
int subsampling(int luma, int plane) {
  return plane > 0 && plane < luma ? 2 : 1;
}

}  // namespace

PlaneMode plane_mode(const ViewState& view) {
  if (view.plane_mode < 0 || view.plane_mode > 4) {
    return PlaneMode::Yuv;
  }
  return static_cast<PlaneMode>(view.plane_mode);
}

unsigned neutral_code(unsigned bit_depth) {
  if (bit_depth == 0) {
    return 0;
  }
  return bit_depth >= 8 ? 128U << (bit_depth - 8U) : 1U << (bit_depth - 1U);
}

pl_rect2df plane_rect(const pl_rect2df& luma_rect, int sub_x, int sub_y) {
  const auto sx = static_cast<float>(sub_x > 0 ? sub_x : 1);
  const auto sy = static_cast<float>(sub_y > 0 ? sub_y : 1);
  return pl_rect2df{luma_rect.x0 / sx, luma_rect.y0 / sy, luma_rect.x1 / sx, luma_rect.y1 / sy};
}

PlaneViews::PlaneViews() {
  replicate_hook_.stages = PL_HOOK_RGB_INPUT;
  replicate_hook_.input = PL_HOOK_SIG_TEX;
  replicate_hook_.hook = replicate_red;
  replicate_hook_.signature = 0x4f56545f504c4e31ULL;  // "OVT_PLN1": a failing hook disables only itself
  hooks_[0] = &replicate_hook_;
}

pl_tex PlaneViews::neutral_tex(pl_gpu gpu, int sample_bytes, unsigned bit_depth, std::string& err) {
  const size_t i = sample_bytes == 1 ? 0 : 1;
  const unsigned value = neutral_code(bit_depth);
  if (neutral_[i] != nullptr && neutral_value_[i] == value) {
    return neutral_[i];
  }
  const char* name = sample_bytes == 1 ? "r8" : "r16";
  pl_fmt fmt = pl_find_named_fmt(gpu, name);
  if (fmt == nullptr) {
    err = std::string("RND-0002 renderer init failed: plane view: no ") + name + " format";
    return nullptr;
  }
  struct pl_tex_params tp = {};
  tp.w = 1;
  tp.h = 1;
  tp.format = fmt;
  tp.sampleable = true;
  tp.host_writable = true;
  // Little-endian, LSB-aligned like the uploaded planes (§10.3.1): the code in the plane's own bit depth.
  const std::array<uint8_t, 2> texel = {static_cast<uint8_t>(value & 0xFFU), static_cast<uint8_t>(value >> 8U)};
  tp.initial_data = texel.data();
  pl_tex_destroy(gpu, &neutral_[i]);
  neutral_[i] = pl_tex_create(gpu, &tp);
  if (neutral_[i] == nullptr) {
    err = std::string("RND-0002 renderer init failed: plane view: cannot create the neutral ") + name + " texture";
    return nullptr;
  }
  neutral_value_[i] = value;
  return neutral_[i];
}

std::string PlaneViews::build_image(pl_gpu gpu, const FrameSlot& slot, const ViewState& view,
                                    const pl_rect2df& luma_rect, struct pl_frame& image) {
  const PlaneMode mode = plane_mode(view);
  image = slot.frame;
  image.crop = luma_rect;
  if (mode == PlaneMode::Yuv) {
    return "";
  }
  const struct pl_bit_encoding bits = slot.frame.repr.bits;
  const int sample_bytes = bits.sample_depth > 8 ? 2 : 1;
  const auto bit_depth = static_cast<unsigned>(bits.color_depth);
  std::string err;

  if (mode == PlaneMode::Gray) {
    // Normal path, chroma replaced by the neutral code: correct luma rendering including tone mapping, no colour. A
    // 4:0:0 picture is already grey; RGB content (matrix 0) has no luma plane to keep, so it renders unchanged.
    if (slot.frame.num_planes < 3 || slot.frame.repr.sys == PL_COLOR_SYSTEM_RGB) {
      return "";
    }
    pl_tex neutral = neutral_tex(gpu, sample_bytes, bit_depth, err);
    if (neutral == nullptr) {
      return err;
    }
    for (int i = 1; i < 3; ++i) {
      image.planes[i].texture = neutral;
      image.planes[i].shift_x = 0.0F;
      image.planes[i].shift_y = 0.0F;
    }
    return "";
  }

  // Modes 1-3: one plane, at its own resolution.
  const int index = static_cast<int>(mode) - 1;
  struct pl_plane plane = {};
  pl_rect2df rect = luma_rect;
  if (index < slot.frame.num_planes) {
    plane = slot.frame.planes[index];
    const pl_tex luma = slot.frame.planes[0].texture;
    rect = plane_rect(luma_rect, subsampling(luma->params.w, plane.texture->params.w),
                      subsampling(luma->params.h, plane.texture->params.h));
  } else {
    // U or V of a 4:0:0 picture: the achromatic value everywhere (mid-grey).
    plane.texture = neutral_tex(gpu, sample_bytes, bit_depth, err);
    if (plane.texture == nullptr) {
      return err;
    }
    rect = pl_rect2df{0.0F, 0.0F, 1.0F, 1.0F};
  }
  // SPEC-ASSUMPTION(#465): §10.6.1 declares the plane as components = 1, component_mapping = {0, -1, -1}.
  // libplacebo merges a plane into the frame by copying only its first `components` channels, each to its mapped
  // channel, so a replicate hook on that plane changes nothing outside R (the picture comes out red). The plane is
  // declared with three components mapped to R, G and B instead: the single-channel texture samples as (r, 0, 0) and
  // the hook makes it (r, r, r) before the merge, which is what §10.6.1 intends.
  plane.components = 3;
  plane.component_mapping[0] = PL_CHANNEL_R;
  plane.component_mapping[1] = PL_CHANNEL_G;
  plane.component_mapping[2] = PL_CHANNEL_B;
  plane.shift_x = 0.0F;  // the plane is shown on its own sample grid: no siting offset
  plane.shift_y = 0.0F;

  image = {};
  image.num_planes = 1;
  image.planes[0] = plane;
  image.crop = rect;
  image.repr = pl_color_repr_rgb;
  image.repr.bits = bits;
  // Y expands the content range (limited 64-940 -> black-white) unless raw_code_values. Chroma is shown on its
  // normalised full range so that the neutral code is mid-grey.
  // SPEC-ASSUMPTION(#465): §10.6.1 says "same construction" for U/V without naming their levels; full
  // range is the only reading under which FR-PLN-001's neutral value maps to mid-grey.
  const bool expand = mode == PlaneMode::Y && !view.raw_code_values;
  image.repr.levels = expand ? slot.frame.repr.levels : PL_COLOR_LEVELS_FULL;
  image.color = pl_color_space_srgb;  // SDR view of the stored codes, never tone-mapped
  return "";
}

void PlaneViews::apply(const ViewState& view, const struct pl_frame& image, struct pl_render_params& params,
                       struct pl_frame& target) const {
  const PlaneMode mode = plane_mode(view);
  if (mode == PlaneMode::Yuv || mode == PlaneMode::Gray) {
    return;
  }
  params.hooks = hooks_.data();
  params.num_hooks = 1;
  if (mode != PlaneMode::Y) {
    params.upscaler = &pl_filter_nearest;  // chroma planes: nearest to the viewport (§10.6.1)
  }
  if (view.raw_code_values) {
    // Pass-through: the same transfer and primaries on both sides, so normalised code values become display values
    // (8-bit targets show code >> (bit_depth - 8), 10-bit targets the top 10 bits). Dithering would move them off the
    // stored codes, so it is off.
    // SPEC-ASSUMPTION(#465): §10.6.1 does not mention dithering; pass-through is only exact without it.
    target.color.primaries = image.color.primaries;
    target.color.transfer = image.color.transfer;
    target.color.hdr = image.color.hdr;
    params.dither_params = nullptr;
  }
}

void PlaneViews::destroy(pl_gpu gpu) {
  for (auto& t : neutral_) {
    pl_tex_destroy(gpu, &t);
  }
  neutral_value_ = {};
}

}  // namespace ovt::render::detail
