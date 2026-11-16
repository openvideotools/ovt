// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Test access to a VideoSurface: CPU download of an OffscreenTexture surface's output (chapter 10 WI-1.400 step 5)
// and the slot A chroma shift (WI-1.401). Internal to libovt-render: the render tests use it, and the PNG export of
// §10.8 will use the download. Not a public header (it lives under render/src/).

#ifndef OVT_RENDER_SRC_SURFACE_OUTPUT_DOWNLOAD_H
#define OVT_RENDER_SRC_SURFACE_OUTPUT_DOWNLOAD_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

struct OutputPixels {
  int width = 0;
  int height = 0;
  size_t texel_bytes = 0;      // 4 for Rgba8Srgb and Rgb10A2, 8 for Rgba16F
  std::vector<uint8_t> bytes;  // width * height * texel_bytes, rows tightly packed, top row first
};

// Waits for the GPU and copies the output texture of `surface` (which must be the VideoSurface of create() with an
// OffscreenTexture target, after render()) into `out`. Returns false, leaving `out` empty, when there is no output.
OVT_RENDER_API bool download_output(const VideoSurface& surface, OutputPixels& out);

// The chroma-plane shift (pl_plane.shift_x/y of plane 1) of the picture in slot A, as set by the chroma siting
// (§10.3.1, WI-1.401). Returns false when slot A holds no picture with chroma planes.
OVT_RENDER_API bool slot_a_chroma_shift(const VideoSurface& surface, float& shift_x, float& shift_y);

// Replaces the millisecond clock the Flicker compare mode reads in render() (steady_clock by default; WI-1.410) and
// restarts the flicker phase at the clock's current value.
OVT_RENDER_API void set_compare_clock(VideoSurface& surface, std::function<int64_t()> clock_ms);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_SURFACE_OUTPUT_DOWNLOAD_H
