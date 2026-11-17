// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Id buffer, asynchronous readback and probe sample windows (chapter 10 §10.5.7; WI-2.435 steps 1-4 and 6).

#include "overlay/idbuffer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

namespace ovt::render::detail {

IdBuffer::IdBuffer(pl_gpu gpu) : gpu_(gpu), region_(!gpu->glsl.vulkan) {}

IdBuffer::~IdBuffer() {
  for (Readback& r : slots_) {
    pl_buf_destroy(gpu_, &r.buf);
  }
  pl_tex_destroy(gpu_, &tex_);
}

bool IdBuffer::begin(int w, int h, std::string& err) {
  pl_fmt fmt = pl_find_named_fmt(gpu_, "r32u");
  constexpr auto kNeeded = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
  if (fmt == nullptr || (fmt->caps & kNeeded) != kNeeded) {
    err = "id buffer format r32u is not renderable and host-readable";
    return false;
  }
  struct pl_tex_params params = {};
  params.w = w;
  params.h = h;
  params.format = fmt;
  params.renderable = true;
  params.host_readable = true;
  params.blit_dst = true;  // pl_tex_clear_ex
  if (!pl_tex_recreate(gpu_, &tex_, &params)) {
    err = "pl_tex_recreate failed for the id buffer";
    return false;
  }
  // SPEC-ASSUMPTION(#712): §10.5.1 clears to 0 (the WI note says 0xFFFFFFFF); 0 is "no id" as fills write block_id + 1.
  union pl_clear_color none = {};
  none.u[0] = kIdNone;
  pl_tex_clear_ex(gpu_, tex_, none);
  return true;
}

void IdBuffer::poll() {
  for (size_t i = 0; i < slots_.size(); ++i) {
    Readback& r = slots_.at(i);
    if (r.pending && !pl_buf_poll(gpu_, r.buf, 0)) {
      r.pending = false;
      if (r.seq >= valid_from_ && (latest_ < 0 || r.seq > slots_.at(static_cast<size_t>(latest_)).seq)) {
        latest_ = static_cast<int>(i);
      }
    }
  }
}

void IdBuffer::readback() {
  if (tex_ == nullptr) {
    return;
  }
  poll();
  Readback* free = nullptr;
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (static_cast<int>(i) != latest_ && !slots_.at(i).pending) {
      free = &slots_.at(i);
      break;
    }
  }
  if (free == nullptr) {
    return;  // both buffers busy or holding the current result: skip this frame
  }
  const int tw = tex_->params.w;
  const int th = tex_->params.h;
  Readback r = *free;
  if (region_) {
    r.x0 = std::clamp(cursor_x_, 0, tw - 1);
    r.y0 = std::clamp(cursor_y_, 0, th - 1);
    r.w = 1;
    r.h = 1;
  } else {
    r.x0 = 0;
    r.y0 = 0;
    r.w = tw;
    r.h = th;
  }
  const size_t size = static_cast<size_t>(r.w) * static_cast<size_t>(r.h) * sizeof(uint32_t);
  if (r.buf == nullptr || r.buf->params.size != size) {
    pl_buf_destroy(gpu_, &r.buf);
    struct pl_buf_params bp = {};
    bp.size = size;
    bp.host_readable = true;
    r.buf = pl_buf_create(gpu_, &bp);
    if (r.buf == nullptr) {
      *free = Readback{};
      return;
    }
  }
  struct pl_tex_transfer_params tp = {};
  tp.tex = tex_;
  tp.rc = {r.x0, r.y0, 0, r.x0 + r.w, r.y0 + r.h, 1};
  tp.row_pitch = static_cast<size_t>(r.w) * sizeof(uint32_t);
  tp.buf = r.buf;
  if (!pl_tex_download(gpu_, &tp)) {
    *free = r;
    free->pending = false;
    free->seq = 0;
    return;
  }
  r.seq = ++seq_;
  r.pending = true;
  *free = r;
}

void IdBuffer::invalidate() {
  latest_ = -1;
  valid_from_ = seq_ + 1;
}

uint32_t IdBuffer::lookup(int px, int py) {
  cursor_x_ = px;
  cursor_y_ = py;
  poll();
  if (latest_ < 0) {
    return kIdNone;
  }
  const Readback& r = slots_.at(static_cast<size_t>(latest_));
  int x = px - r.x0;
  int y = py - r.y0;
  if (region_) {
    x = 0;  // the texel read at the previous cursor (§10.5.7: pick() reflects the previous frame's cursor)
    y = 0;
  } else if (x < 0 || y < 0 || x >= r.w || y >= r.h) {
    return kIdNone;
  }
  uint32_t id = kIdNone;
  const size_t offset = ((static_cast<size_t>(y) * static_cast<size_t>(r.w)) + static_cast<size_t>(x)) * sizeof(id);
  if (!pl_buf_read(gpu_, r.buf, offset, &id, sizeof(id))) {
    return kIdNone;
  }
  return id;
}

bool IdBuffer::wait(uint64_t timeout_ns) {
  // pl_buf_poll returns once any command completes, not necessarily the download, so poll until the deadline.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
  for (;;) {
    bool busy = false;
    for (const Readback& r : slots_) {
      busy = busy || (r.pending && pl_buf_poll(gpu_, r.buf, 1'000'000));
    }
    if (!busy) {
      poll();
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
  }
}

// ---- probe sample windows --------------------------------------------------------------------------------------

bool SampleWindows::fetch(pl_gpu gpu, const FrameSlot& slot, int ix, int iy) {
  have_ = false;
  for (size_t p = 0; p < planes_.size(); ++p) {
    Window& win = planes_.at(p);
    win = Window{};
    pl_tex tex = slot.tex.at(p);
    if (tex == nullptr) {
      continue;
    }
    // Chroma planes: 4:2:0 halves both axes, 4:2:2 the width; the sample covering (ix, iy) is co-sited per siting.
    const bool chroma = p > 0;
    const int sx = chroma && (slot.chroma_format == 1 || slot.chroma_format == 2) ? 1 : 0;
    const int sy = chroma && slot.chroma_format == 1 ? 1 : 0;
    const int tw = tex->params.w;
    const int th = tex->params.h;
    win.w = std::min(kSize, tw);
    win.h = std::min(kSize, th);
    win.x0 = std::clamp((ix >> sx) - (kSize / 2), 0, tw - win.w);
    win.y0 = std::clamp((iy >> sy) - (kSize / 2), 0, th - win.h);
    const size_t texel = tex->params.format->texel_size;
    const size_t bytes = static_cast<size_t>(win.w) * static_cast<size_t>(win.h) * texel;
    // Through an explicit host-readable buffer, as the scope readback: libplacebo's pointer download path crashes on
    // MoltenVK with an imported device.
    struct pl_buf_params bp = {};
    bp.size = bytes;
    bp.host_readable = true;
    pl_buf buf = pl_buf_create(gpu, &bp);
    if (buf == nullptr) {
      return false;
    }
    struct pl_tex_transfer_params tp = {};
    tp.tex = tex;
    tp.rc = {win.x0, win.y0, 0, win.x0 + win.w, win.y0 + win.h, 1};
    tp.row_pitch = static_cast<size_t>(win.w) * texel;
    tp.buf = buf;
    std::vector<uint8_t> raw(bytes);
    const bool ok = pl_tex_download(gpu, &tp) && pl_buf_read(gpu, buf, 0, raw.data(), bytes);
    pl_buf_destroy(gpu, &buf);
    if (!ok) {
      return false;
    }
    win.samples.resize(static_cast<size_t>(win.w) * static_cast<size_t>(win.h));
    for (size_t i = 0; i < win.samples.size(); ++i) {
      // R8, or R16 holding little-endian LSB-aligned codes (§10.3.1).
      win.samples[i] = texel == 1 ? raw[i] : static_cast<uint16_t>(raw[2 * i] | (raw[(2 * i) + 1] << 8U));
    }
  }
  ++downloads_;
  generation_ = slot.generation;
  luma_ = slot.tex[0];
  centre_x_ = ix;
  centre_y_ = iy;
  have_ = true;
  return true;
}

bool SampleWindows::read(pl_gpu gpu, const FrameSlot& slot, int ix, int iy, std::array<int32_t, 3>& code) {
  code = {-1, -1, -1};
  if (!slot.valid || slot.tex[0] == nullptr) {
    return false;
  }
  // §10.5.7 re-downloads past 32 samples; at exactly 32 the cursor would already be on the window's far edge.
  const bool stale = !have_ || generation_ != slot.generation || luma_ != slot.tex[0] ||
                     std::abs(ix - centre_x_) >= kSize / 2 || std::abs(iy - centre_y_) >= kSize / 2;
  if (stale && !fetch(gpu, slot, ix, iy)) {
    return false;
  }
  bool any = false;
  for (size_t p = 0; p < planes_.size(); ++p) {
    const Window& win = planes_.at(p);
    if (win.samples.empty()) {
      continue;
    }
    const bool chroma = p > 0;
    const int sx = chroma && (slot.chroma_format == 1 || slot.chroma_format == 2) ? 1 : 0;
    const int sy = chroma && slot.chroma_format == 1 ? 1 : 0;
    const int x = (ix >> sx) - win.x0;
    const int y = (iy >> sy) - win.y0;
    if (x < 0 || y < 0 || x >= win.w || y >= win.h) {
      continue;  // outside the plane (a picture smaller than the cursor position)
    }
    code.at(p) = win.samples[(static_cast<size_t>(y) * static_cast<size_t>(win.w)) + static_cast<size_t>(x)];
    any = true;
  }
  return any;
}

}  // namespace ovt::render::detail
