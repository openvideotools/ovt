// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The id buffer and the CPU side of VideoSurface::pick() (chapter 10 §10.5.1, §10.5.6, §10.5.7; WI-2.435): a
// drawable-size R32_UINT texture the fill and arrow passes write ids into, its double-buffered asynchronous readback,
// and the 64x64-sample code-value windows the probe reads. Internal to libovt-render; render thread only.

#ifndef OVT_RENDER_SRC_OVERLAY_IDBUFFER_H
#define OVT_RENDER_SRC_OVERLAY_IDBUFFER_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

#include "ovt/render/render_api.h"
#include "pipeline/frame_upload.h"

namespace ovt::render::detail {

// Id encoding (§10.5.6): 0 = nothing, block_id + 1 from the fill pass, kIdArrowBit | mv_index from the arrow pass.
inline constexpr uint32_t kIdNone = 0;
inline constexpr uint32_t kIdArrowBit = 0x80000000U;

class IdBuffer {
 public:
  explicit IdBuffer(pl_gpu gpu);
  IdBuffer(const IdBuffer&) = delete;
  IdBuffer& operator=(const IdBuffer&) = delete;
  IdBuffer(IdBuffer&&) = delete;
  IdBuffer& operator=(IdBuffer&&) = delete;
  ~IdBuffer();

  // Sizes the texture to w x h and clears it to kIdNone (every frame). False with `err` set on failure.
  bool begin(int w, int h, std::string& err);
  [[nodiscard]] pl_tex target() const { return tex_; }

  // After the frame's id passes: downloads the texture (or, in region mode, the texel under the last cursor) into a
  // free readback buffer. A buffer is reused only once pl_buf_poll reports it complete; when none is free the frame
  // is not read back (the previous result stays current).
  void readback();
  // The frame drew no ids: forget every result, including downloads still in flight.
  void invalidate();

  // The id at device pixel (px, py) of the newest completed readback (at most one frame stale), kIdNone when there
  // is none. Never blocks. In region mode it returns the texel read at the previous cursor and remembers (px, py)
  // for the next readback (§10.5.7 GLES fallback).
  uint32_t lookup(int px, int py);

  // Region mode (step 6): 1x1 downloads at the cursor instead of the whole texture. Chosen for backends other than
  // Vulkan (GLES without host-mapped buffers); tests force it.
  void set_region_mode(bool region) { region_ = region; }
  [[nodiscard]] bool region_mode() const { return region_; }

  // Tests: waits up to `timeout_ns` for the downloads in flight. True when none is left pending.
  bool wait(uint64_t timeout_ns);

 private:
  struct Readback {
    pl_buf buf = nullptr;
    int w = 0;  // texels of the download (1 x 1 in region mode)
    int h = 0;
    int x0 = 0;  // origin of the download in the texture
    int y0 = 0;
    uint64_t seq = 0;
    bool pending = false;
  };
  void poll();

  pl_gpu gpu_;
  pl_tex tex_ = nullptr;
  std::array<Readback, 2> slots_{};
  int latest_ = -1;          // newest completed readback, -1 none
  uint64_t seq_ = 0;         // downloads issued
  uint64_t valid_from_ = 1;  // downloads with seq below this were invalidated
  int cursor_x_ = 0;
  int cursor_y_ = 0;
  bool region_ = false;
};

// The 64 x 64-sample windows of a picture slot's planes around the probe position (§10.5.7). A window is downloaded
// again when the slot's picture changes or the cursor moves more than 32 samples from the window centre; Picture
// memory is not kept by the surface, so the textures are the source of truth.
class OVT_RENDER_API SampleWindows {
 public:
  // The code values of `slot` at luma sample (ix, iy): Y, U, V (chroma: the sample covering (ix, iy) at the slot's
  // subsampling); -1 for a plane the slot does not have. False when nothing can be read.
  bool read(pl_gpu gpu, const FrameSlot& slot, int ix, int iy, std::array<int32_t, 3>& code);
  void reset() { planes_ = {}; }
  [[nodiscard]] uint64_t downloads() const { return downloads_; }  // tests: windows downloaded so far

  static constexpr int kSize = 64;

 private:
  struct Window {
    int x0 = 0;
    int y0 = 0;
    int w = 0;
    int h = 0;
    std::vector<uint16_t> samples;
  };
  bool fetch(pl_gpu gpu, const FrameSlot& slot, int ix, int iy);

  std::array<Window, 3> planes_{};
  uint64_t generation_ = 0;
  pl_tex luma_ = nullptr;  // identity of the slot's luma texture the windows came from
  int centre_x_ = 0;
  int centre_y_ = 0;
  bool have_ = false;
  uint64_t downloads_ = 0;
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_IDBUFFER_H
