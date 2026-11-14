// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Picture -> pl_frame / pl_plane_data (chapter 10 §10.3.1, WI-1.401). Internal to libovt-render.

#ifndef OVT_RENDER_SRC_PIPELINE_FRAME_UPLOAD_H
#define OVT_RENDER_SRC_PIPELINE_FRAME_UPLOAD_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "color/h273_map.h"
#include "ovt/render/picture.h"

namespace ovt::render::detail {

// One picture slot (A or B): up to three persistent plane textures and the pl_frame that samples them.
struct FrameSlot {
  std::array<pl_tex, 3> tex{};
  struct pl_frame frame{};
  // Coded picture size and conformance window of the uploaded picture (luma samples).
  uint32_t coded_width = 0;
  uint32_t coded_height = 0;
  ovt::fb::Rect crop;
  uint8_t chroma_format = 1;  // of the uploaded picture (0 4:0:0, 1 4:2:0, 2 4:2:2, 3 4:4:4); compare slot B (WI-1.410)
  uint8_t bit_depth = 8;
  uint64_t generation = 0;
  bool valid = false;  // a picture has been uploaded
};

// The R-10.2.1 checks. Returns "" for a usable picture, otherwise the RND-0001 message (§10.10).
std::string validate_picture(const Picture& pic);

// §10.3.1 chroma siting table: ovt_ColorDescription.chroma_siting -> pl_chroma_location.
enum pl_chroma_location chroma_location(uint8_t chroma_siting);

// Uploads `pic` into `slot` (validate first). Skips the upload when the slot already holds a picture with the same
// generation (§10.2.1). Returns "" on success, otherwise an RND-0001 message; on failure the slot is left invalid.
// The colour description is mapped by the §10.3.3 tables (color/h273_map.h); the RND-0020/RND-0021 lines that
// `notice_log` has not yet seen for this description are appended to `notices`.
std::string upload_picture(pl_gpu gpu, const Picture& pic, FrameSlot& slot, H273NoticeLog& notice_log,
                           std::vector<std::string>& notices);

// Source rectangle of the slot for the current view: the conformance window when show_cropped and the window is
// non-empty, otherwise the full coded picture (§10.3.1 "Crop").
pl_rect2df source_rect(const FrameSlot& slot, bool show_cropped);

void destroy_slot(pl_gpu gpu, FrameSlot& slot);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PIPELINE_FRAME_UPLOAD_H
