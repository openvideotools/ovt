// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Reconstruction planes of the AV1 analysis plugin: libaom's external frame buffers kept alive for the PlaneRefs,
// and the PlaneRefs themselves (chapter 07 §7.6.1 plane rows, WI-2.158).

#include "planes.hpp"

#include <aom/aom_frame_buffer.h>
#include <aom/aom_image.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace ovt::aom {

/// One frame buffer. `in_libaom` is true between get() and release(); a Picture holds a shared_ptr copy.
struct FramePool::Buffer {
  std::vector<uint8_t> data;
  bool in_libaom = false;
};

int FramePool::get(void* pool, size_t min_size, aom_codec_frame_buffer* fb) noexcept {
  if (pool == nullptr || fb == nullptr) {
    return -1;
  }
  auto& self = *static_cast<FramePool*>(pool);
  try {
    const std::lock_guard<std::mutex> lock(self.mu_);
    // A buffer is free when libaom has released it and no Picture holds it (the pool's own reference is the only
    // one; Pictures are only made under the lock, from the pool's shared_ptr). Free buffers that are too small for
    // this request are dropped, so the pool follows resolution changes instead of growing.
    auto& bufs = self.buffers_;
    std::erase_if(bufs, [min_size](const std::shared_ptr<Buffer>& b) {
      return !b->in_libaom && b.use_count() == 1 && b->data.size() < min_size;
    });
    const auto it = std::find_if(bufs.begin(), bufs.end(),
                                 [](const std::shared_ptr<Buffer>& b) { return !b->in_libaom && b.use_count() == 1; });
    Buffer* buf = nullptr;
    if (it != bufs.end()) {
      buf = it->get();
      std::fill(buf->data.begin(), buf->data.end(), uint8_t{0});  // libaom requires zeroed memory
    } else {
      auto fresh = std::make_shared<Buffer>();
      fresh->data.assign(min_size, 0);
      buf = fresh.get();
      bufs.push_back(std::move(fresh));
    }
    buf->in_libaom = true;
    fb->data = buf->data.data();
    fb->size = buf->data.size();
    fb->priv = buf;
    return 0;
  } catch (...) {
    return -1;  // out of memory: libaom reports AOM_CODEC_MEM_ERROR and the frame fails to decode
  }
}

int FramePool::release(void* pool, aom_codec_frame_buffer* fb) noexcept {
  if (pool == nullptr || fb == nullptr || fb->priv == nullptr) {
    return 0;  // libaom releases never-allocated slots too
  }
  auto& self = *static_cast<FramePool*>(pool);
  const std::lock_guard<std::mutex> lock(self.mu_);
  for (const auto& b : self.buffers_) {
    if (b.get() == fb->priv) {
      b->in_libaom = false;
      return 0;
    }
  }
  return -1;
}

Picture FramePool::retain(const aom_image& img) {
  Picture pic;
  const std::lock_guard<std::mutex> lock(mu_);
  const auto it = std::find_if(buffers_.begin(), buffers_.end(),
                               [&img](const std::shared_ptr<Buffer>& b) { return b.get() == img.fb_priv; });
  if (it == buffers_.end()) {
    return pic;
  }
  pic.owner = *it;
  pic.num_planes = img.monochrome != 0 ? 1 : 3;
  const uint8_t sample_bytes = (static_cast<unsigned>(img.fmt) & AOM_IMG_FMT_HIGHBITDEPTH) != 0 ? 2 : 1;
  for (int p = 0; p < pic.num_planes; ++p) {
    const unsigned sx = p == 0 ? 0 : img.x_chroma_shift;
    const unsigned sy = p == 0 ? 0 : img.y_chroma_shift;
    PlaneView& v = pic.planes.at(static_cast<size_t>(p));
    v.data = img.planes[p];
    v.width = (img.d_w + sx) >> sx;
    v.height = (img.d_h + sy) >> sy;
    v.stride_bytes = static_cast<uint32_t>(img.stride[p]);
    v.bit_depth = static_cast<uint8_t>(img.bit_depth);
    v.sample_bytes = sample_bytes;
  }
  return pic;
}

size_t FramePool::held_by_pictures() const {
  const std::lock_guard<std::mutex> lock(mu_);
  return static_cast<size_t>(std::count_if(buffers_.begin(), buffers_.end(), [](const std::shared_ptr<Buffer>& b) {
    return !b->in_libaom && b.use_count() > 1;
  }));
}

std::string blob_id(int32_t decode_index, ovt::fb::PlaneStage stage, int plane) {
  // Chapter 06 §6.6.2 stage tokens, in ovt.fb.PlaneStage order.
  static constexpr std::array<const char*, 7> kStageTokens = {"recon",      "pred",  "resid", "predeblock",
                                                              "presaocdef", "prelr", "coeff"};
  static constexpr std::array<const char*, 3> kPlaneNames = {"y", "u", "v"};
  return std::to_string(decode_index) + "/" + kStageTokens.at(static_cast<size_t>(stage)) + "/" +
         kPlaneNames.at(static_cast<size_t>(plane));
}

void add_plane_refs(flatbuffers::FlatBufferBuilder& fbb, int32_t decode_index, const Picture& recon,
                    uint32_t plane_stage_mask, std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>>* out,
                    std::map<std::string, PlaneView>* blobs) {
  constexpr auto kRecon = ovt::fb::PlaneStage::Reconstruction;
  if ((plane_stage_mask & (1U << static_cast<unsigned>(kRecon))) == 0 || recon.empty()) {
    return;
  }
  for (int p = 0; p < recon.num_planes; ++p) {
    const PlaneView& v = recon.planes.at(static_cast<size_t>(p));
    std::string id = blob_id(decode_index, kRecon, p);
    out->push_back(ovt::fb::CreatePlaneRefDirect(fbb, kRecon, static_cast<uint8_t>(p), v.width, v.height,
                                                 v.stride_bytes, v.bit_depth, v.sample_bytes, /*is_signed=*/false,
                                                 id.c_str()));
    blobs->emplace(std::move(id), v);
  }
}

}  // namespace ovt::aom
