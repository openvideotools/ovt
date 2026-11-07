// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Picture -> pl_frame / pl_plane_data (chapter 10 §10.3.1, WI-1.401).

#include "pipeline/frame_upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ovt::render::detail {
namespace {

std::string depth_reject(unsigned bit_depth, unsigned sample_bytes) {
  return "RND-0001 picture rejected: bit_depth " + std::to_string(bit_depth) + " / sample_bytes " +
         std::to_string(sample_bytes) + " unsupported";
}

// SPEC-ASSUMPTION(#209): §10.10 gives RND-0001 one template (bit depth / sample size). The other R-10.2.1 violations
// (plane sizes, strides, null data, crop) are rejected under the same code with their own reason, because uploading
// them would read outside the caller's buffers.
std::string reject(const std::string& reason) {
  return "RND-0001 picture rejected: " + reason;
}

uint32_t ceil_div(uint32_t v, uint32_t d) {
  return (v + d - 1) / d;
}

}  // namespace

namespace {

// R-10.2.1 for one plane: sample size, size against the coded size and chroma format, data, stride.
std::string validate_plane(const Picture& pic, uint8_t i) {
  const ovt_ColorDescription& c = pic.color;
  const PlaneData& p = pic.planes[i];
  if ((p.sample_bytes != 1 && p.sample_bytes != 2) || p.sample_bytes != pic.planes[0].sample_bytes ||
      (c.bit_depth > 8 && p.sample_bytes != 2)) {
    return depth_reject(c.bit_depth, p.sample_bytes);
  }
  const uint32_t sub_x = (c.chroma_format == 1 || c.chroma_format == 2) ? 2 : 1;
  const uint32_t sub_y = c.chroma_format == 1 ? 2 : 1;
  const uint32_t want_w = i == 0 ? pic.coded_width : ceil_div(pic.coded_width, sub_x);
  const uint32_t want_h = i == 0 ? pic.coded_height : ceil_div(pic.coded_height, sub_y);
  if (p.width != want_w || p.height != want_h) {
    return reject("plane " + std::to_string(i) + " is " + std::to_string(p.width) + "x" + std::to_string(p.height) +
                  ", expected " + std::to_string(want_w) + "x" + std::to_string(want_h));
  }
  if (p.data == nullptr) {
    return reject("plane " + std::to_string(i) + " has no data");
  }
  if (p.stride_bytes < uint64_t{p.width} * p.sample_bytes || p.stride_bytes % p.sample_bytes != 0) {
    return reject("plane " + std::to_string(i) + " stride " + std::to_string(p.stride_bytes) + " is invalid");
  }
  return "";
}

// An empty crop (w = h = 0) means the whole coded picture; any other crop must lie inside it.
bool crop_valid(const Picture& pic) {
  const ovt::fb::Rect& r = pic.crop;
  if (r.w() == 0 && r.h() == 0) {
    return true;
  }
  return r.x() >= 0 && r.y() >= 0 && r.w() > 0 && r.h() > 0 && int64_t{r.x()} + r.w() <= int64_t{pic.coded_width} &&
         int64_t{r.y()} + r.h() <= int64_t{pic.coded_height};
}

}  // namespace

std::string validate_picture(const Picture& pic) {
  const ovt_ColorDescription& c = pic.color;
  if (c.bit_depth == 0 || c.bit_depth > 16) {
    return depth_reject(c.bit_depth, pic.planes[0].sample_bytes);
  }
  if (c.chroma_format > 3) {
    return reject("chroma_format " + std::to_string(c.chroma_format) + " is not 0..3");
  }
  const uint8_t expected_planes = c.chroma_format == 0 ? 1 : 3;
  if (pic.num_planes != expected_planes) {
    return reject("num_planes " + std::to_string(pic.num_planes) + " does not match chroma_format " +
                  std::to_string(c.chroma_format));
  }
  if (pic.coded_width == 0 || pic.coded_height == 0) {
    return reject("coded size is empty");
  }
  for (uint8_t i = 0; i < pic.num_planes; ++i) {
    std::string err = validate_plane(pic, i);
    if (!err.empty()) {
      return err;
    }
  }
  if (!crop_valid(pic)) {
    return reject("crop rectangle lies outside the coded picture");
  }
  return "";
}

enum pl_chroma_location chroma_location(uint8_t chroma_siting) {
  switch (chroma_siting) {
    case 2:
      return PL_CHROMA_CENTER;
    case 3:
      return PL_CHROMA_TOP_LEFT;
    default:
      // 1 left; 0 unknown: H.273's default chroma_sample_loc_type 0 for H.264/HEVC, and dav1d/libplacebo's convention
      // for AV1 CSP_UNKNOWN.
      return PL_CHROMA_LEFT;
  }
}

void fill_color(const Picture& pic, struct pl_frame& frame) {
  // TODO(WI-1.402): replace with the §10.3.3 tables of render/src/color/h273_map.cpp.
  const ovt_ColorDescription& c = pic.color;
  const auto w = static_cast<int>(pic.coded_width);
  const auto h = static_cast<int>(pic.coded_height);
  frame.repr = pl_color_repr_unknown;
  frame.repr.levels = c.full_range != 0 ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  switch (c.matrix) {
    case 0:
      frame.repr.sys = PL_COLOR_SYSTEM_RGB;
      break;
    case 1:
      frame.repr.sys = PL_COLOR_SYSTEM_BT_709;
      break;
    case 5:
    case 6:
      frame.repr.sys = PL_COLOR_SYSTEM_BT_601;
      break;
    case 9:
      frame.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
      break;
    case 10:
      frame.repr.sys = PL_COLOR_SYSTEM_BT_2020_C;
      break;
    default:
      frame.repr.sys = pl_color_system_guess_ycbcr(w, h);
      break;
  }
  frame.color = pl_color_space_unknown;
  switch (c.primaries) {
    case 1:
      frame.color.primaries = PL_COLOR_PRIM_BT_709;
      break;
    case 9:
      frame.color.primaries = PL_COLOR_PRIM_BT_2020;
      break;
    case 12:
      frame.color.primaries = PL_COLOR_PRIM_DISPLAY_P3;
      break;
    default:
      frame.color.primaries = pl_color_primaries_guess(w, h);
      break;
  }
  switch (c.transfer) {
    case 8:
      frame.color.transfer = PL_COLOR_TRC_LINEAR;
      break;
    case 13:
      frame.color.transfer = PL_COLOR_TRC_SRGB;
      break;
    case 16:
      frame.color.transfer = PL_COLOR_TRC_PQ;
      break;
    case 18:
      frame.color.transfer = PL_COLOR_TRC_HLG;
      break;
    default:
      frame.color.transfer = PL_COLOR_TRC_BT_1886;
      break;
  }
}

namespace {

// Creates (or reuses) the plane texture of slot.tex[i], fills it from the caller's buffer and describes it in
// frame.planes[i]. Returns the texture's component depth (8 or 16), or 0 with err set.
int upload_plane(pl_gpu gpu, const Picture& pic, uint8_t i, FrameSlot& slot, struct pl_frame& frame, std::string& err) {
  const PlaneData& p = pic.planes[i];
  const char* fmt_name = p.sample_bytes == 1 ? "r8" : "r16";
  // R8 or R16 UNORM, chosen by name. pl_upload_plane()'s format search can land on a format whose samples are not
  // LSB-aligned (lavapipe offers R10X6, which keeps the value in the high bits), and the surface must never pre-shift
  // samples (§10.3.1), so the plane texture is created and filled directly.
  pl_fmt fmt = pl_find_named_fmt(gpu, fmt_name);
  if (fmt == nullptr || (fmt->caps & PL_FMT_CAP_SAMPLEABLE) == 0 || fmt->texel_size != p.sample_bytes) {
    err = reject(std::string("the GPU has no sampleable ") + fmt_name + " format");
    return 0;
  }
  struct pl_tex_params tp = {};
  tp.w = static_cast<int>(p.width);
  tp.h = static_cast<int>(p.height);
  tp.format = fmt;
  tp.sampleable = true;
  tp.host_writable = true;
  // Reallocates slot.tex[i] only when the size or format changes (persistent textures per slot).
  if (!pl_tex_recreate(gpu, &slot.tex[i], &tp)) {
    err = reject("cannot allocate the texture of plane " + std::to_string(i));
    return 0;
  }
  struct pl_tex_transfer_params xfer = {};
  xfer.tex = slot.tex[i];
  xfer.row_pitch = p.stride_bytes;
  xfer.ptr = const_cast<void*>(p.data);  // NOLINT(cppcoreguidelines-pro-type-const-cast): uploads only read it
  if (!pl_tex_upload(gpu, &xfer)) {
    err = reject("upload of plane " + std::to_string(i) + " failed");
    return 0;
  }
  // Matrix 0 (identity, H.273) stores the planes as G, B, R; every other matrix as Y, Cb, Cr (PL_CHANNEL_Y/CB/CR).
  static constexpr std::array<int, 3> kGbr = {PL_CHANNEL_G, PL_CHANNEL_B, PL_CHANNEL_R};
  struct pl_plane& plane = frame.planes[i];
  plane.texture = slot.tex[i];
  plane.components = 1;
  plane.component_mapping[0] = pic.color.matrix == 0 ? kGbr.at(i) : static_cast<int>(i);
  plane.shift_x = 0;
  plane.shift_y = 0;
  plane.flipped = false;
  return fmt->component_depth[0];
}

}  // namespace

std::string upload_picture(pl_gpu gpu, const Picture& pic, FrameSlot& slot) {
  if (slot.valid && slot.generation == pic.generation) {
    return "";  // §10.2.1: same generation for the same slot -> no re-upload
  }
  std::string err = validate_picture(pic);
  if (!err.empty()) {
    slot.valid = false;
    return err;
  }

  struct pl_frame frame = {};
  frame.num_planes = pic.num_planes;
  int depth = 0;
  for (uint8_t i = 0; i < pic.num_planes; ++i) {
    const int plane_depth = upload_plane(gpu, pic, i, slot, frame, err);
    if (plane_depth == 0) {
      slot.valid = false;
      return err;
    }
    if (i == 0) {
      depth = plane_depth;  // 8 or 16: what the texture holds (§10.11 WI-1.401 notes)
    }
  }
  for (uint8_t i = pic.num_planes; i < 3; ++i) {
    pl_tex_destroy(gpu, &slot.tex[i]);
  }

  fill_color(pic, frame);
  // Bit depth is declared on the frame (§10.3.1); samples are LSB-aligned and never pre-shifted.
  frame.repr.bits.sample_depth = depth;
  frame.repr.bits.color_depth = pic.color.bit_depth;
  frame.repr.bits.bit_shift = 0;
  if (pic.num_planes == 3) {
    pl_frame_set_chroma_location(&frame, chroma_location(pic.color.chroma_siting));
  }

  slot.frame = frame;
  slot.coded_width = pic.coded_width;
  slot.coded_height = pic.coded_height;
  slot.crop = pic.crop;
  slot.generation = pic.generation;
  slot.valid = true;
  return "";
}

pl_rect2df source_rect(const FrameSlot& slot, bool show_cropped) {
  const ovt::fb::Rect& r = slot.crop;
  if (show_cropped && r.w() > 0 && r.h() > 0) {
    return pl_rect2df{static_cast<float>(r.x()), static_cast<float>(r.y()), static_cast<float>(r.x() + r.w()),
                      static_cast<float>(r.y() + r.h())};
  }
  return pl_rect2df{0.0F, 0.0F, static_cast<float>(slot.coded_width), static_cast<float>(slot.coded_height)};
}

void destroy_slot(pl_gpu gpu, FrameSlot& slot) {
  for (auto& t : slot.tex) {
    pl_tex_destroy(gpu, &t);
  }
  slot = FrameSlot{};
}

}  // namespace ovt::render::detail
