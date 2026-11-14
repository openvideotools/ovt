// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Planes of the AV1 analysis plugin (chapter 07 §7.6.1 plane rows, chapter 06 §6.6, WI-2.158). The Reconstruction
// of a shown frame is the aom_image_t that aom_codec_get_frame() returns after the aom_codec_decode() call that
// decoded it. That image lives in a frame buffer libaom takes from FramePool, the plugin's external frame-buffer
// pool, which keeps the buffer alive after libaom lets go of it for as long as a Picture refers to it. So the
// samples are never copied: the PlaneRefs of a pulled FrameAnalysis point into the frame buffer itself, and a pull
// without the Reconstruction stage just drops the Picture.
//
// Free of libaom headers (the aom types are forward-declared), so that fb_builder.cpp and the unit tests that
// build frames by hand need none.
#ifndef OVT_PLUGIN_AOM_PLANES_HPP
#define OVT_PLUGIN_AOM_PLANES_HPP

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ovt_model_generated.h"

struct aom_codec_frame_buffer;  // aom/aom_frame_buffer.h: aom_codec_frame_buffer_t
struct aom_image;               // aom/aom_image.h: aom_image_t

namespace ovt::aom {

/// One plane in place: rows of width × sample_bytes bytes, stride_bytes apart (chapter 06 §6.6.1 rule 3), 16-bit
/// samples little-endian with the value in the low bit_depth bits (rule 2).
struct PlaneView {
  const uint8_t* data = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  uint8_t bit_depth = 8;
  uint8_t sample_bytes = 1;

  /// Bytes from the first sample to the last one: the size plane() reports for this plane.
  [[nodiscard]] size_t span_bytes() const {
    return height == 0 ? 0 : size_t{stride_bytes} * (height - 1) + size_t{width} * sample_bytes;
  }
};

/// The output image of one shown frame: up to three planes (one for 4:0:0) and the frame buffer they live in.
/// Copies share the buffer; it goes back to FramePool when libaom has released it and the last copy is gone.
struct Picture {
  std::shared_ptr<const void> owner;  // null: no picture
  std::array<PlaneView, 3> planes{};
  int num_planes = 0;

  [[nodiscard]] bool empty() const { return owner == nullptr || num_planes == 0; }
};

/// libaom's external frame buffers (aom_codec_set_frame_buffer_functions), reference-counted so that a decoded
/// picture can outlive the next aom_codec_decode() call. libaom calls get() and release() with the pool as priv,
/// possibly from its worker threads, always under its buffer-pool lock; the pool has its own lock for retain()
/// and the test counters, which run on the plugin's thread.
class FramePool {
 public:
  FramePool() = default;
  ~FramePool() = default;
  FramePool(const FramePool&) = delete;
  FramePool& operator=(const FramePool&) = delete;
  FramePool(FramePool&&) = delete;
  FramePool& operator=(FramePool&&) = delete;

  /// aom_get_frame_buffer_cb_fn_t: a zeroed buffer of at least @p min_size bytes, reused when one is free.
  static int get(void* pool, size_t min_size, aom_codec_frame_buffer* fb) noexcept;
  /// aom_release_frame_buffer_cb_fn_t: libaom no longer uses the buffer; it stays alive while a Picture holds it.
  static int release(void* pool, aom_codec_frame_buffer* fb) noexcept;

  /// The planes of @p img (an image from aom_codec_get_frame() whose frame buffer came from this pool) as a
  /// Picture that holds the buffer. Width and height are the image's d_w × d_h, i.e. the upscaled output size of a
  /// super-resolution frame (§7.6.1); chroma is rounded up by the subsampling. Empty when the buffer is not ours.
  Picture retain(const aom_image& img);

  /// Buffers that libaom has released but a Picture still holds (for tests: zero once every Picture is gone).
  [[nodiscard]] size_t held_by_pictures() const;

 private:
  struct Buffer;
  mutable std::mutex mu_;
  std::vector<std::shared_ptr<Buffer>> buffers_;
};

/// The plugin-local blob id "<decode_index>/<stage>/<y|u|v>" with the chapter 06 §6.6.2 stage token.
/// SPEC-ASSUMPTION(#250): the host prefixes the stream's source hash to make the §6.6.2 blob_id.
std::string blob_id(int32_t decode_index, ovt::fb::PlaneStage stage, int plane);

/// Adds the PlaneRefs of @p recon (stage Reconstruction) to @p out when @p plane_stage_mask asks for that stage, and
/// each one's blob id with its PlaneView to @p blobs. The views point into @p recon's frame buffer, so whoever keeps
/// @p blobs must keep @p recon too.
void add_plane_refs(flatbuffers::FlatBufferBuilder& fbb, int32_t decode_index, const Picture& recon,
                    uint32_t plane_stage_mask, std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>>* out,
                    std::map<std::string, PlaneView>* blobs);

/// The same for a picture of stage @p stage (the PreDeblock, PreSaoOrCdef and PreLoopRestoration snapshots of
/// extension patch 0002, WI-2.159).
void add_stage_plane_refs(flatbuffers::FlatBufferBuilder& fbb, int32_t decode_index, ovt::fb::PlaneStage stage,
                          const Picture& pic, uint32_t plane_stage_mask,
                          std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>>* out,
                          std::map<std::string, PlaneView>* blobs);

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_PLANES_HPP
