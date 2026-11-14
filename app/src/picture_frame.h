// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PictureFrame (WI-1.405): one decoded picture that owns its samples, handed from the GUI thread to the PictureHost
// renderer on the render thread as std::shared_ptr<const PictureFrame>. The ovt::render::Picture inside points into
// `planes`, so a frame is immutable once made and may be shared by several hosts (dual view, WI-2.489). The decoder
// path (ovt_decoder_next, WI-2.171; ovt_analysis_plane) fills one through make_picture_frame().

#ifndef OVT_APP_PICTURE_FRAME_H
#define OVT_APP_PICTURE_FRAME_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ovt/color.h"
#include "ovt/render/picture.h"

namespace ovt::app {

struct PictureFrame {
  ovt::render::Picture picture;                     // planes[i].data point into planes[i] below
  std::array<std::vector<std::uint8_t>, 3> planes;  // tightly packed rows (stride = width * sample_bytes)

  PictureFrame() = default;
  PictureFrame(const PictureFrame&) = delete;
  PictureFrame& operator=(const PictureFrame&) = delete;
  PictureFrame(PictureFrame&&) = delete;
  PictureFrame& operator=(PictureFrame&&) = delete;
  ~PictureFrame() = default;

  // Sets one sample (value in the low bit_depth bits; little-endian for 2-byte samples).
  void set(std::size_t plane, std::uint32_t x, std::uint32_t y, std::uint32_t value) {
    const ovt::render::PlaneData& p = picture.planes[plane];
    const std::size_t at =
        (static_cast<std::size_t>(y) * p.stride_bytes) + (static_cast<std::size_t>(x) * p.sample_bytes);
    std::vector<std::uint8_t>& buf = planes.at(plane);
    buf.at(at) = static_cast<std::uint8_t>(value & 0xFFU);
    if (p.sample_bytes == 2) {
      buf.at(at + 1) = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    }
  }
};

// A new generation number for Picture::generation: unique per process, so a host never mistakes a new frame for the
// one it already uploaded (§10.2.1 skip rule).
inline std::uint64_t next_picture_generation() {
  static std::atomic<std::uint64_t> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

// An all-zero frame of coded_width x coded_height with the planes §10.2.1 requires for `color` (chroma_format and
// bit_depth decide the plane sizes and sample bytes) and a fresh generation. Returns nullptr for a size of 0 or a
// bit depth above 16.
inline std::shared_ptr<PictureFrame> make_picture_frame(std::uint32_t coded_width, std::uint32_t coded_height,
                                                        const ovt_ColorDescription& color) {
  if (coded_width == 0 || coded_height == 0 || color.bit_depth == 0 || color.bit_depth > 16) {
    return nullptr;
  }
  auto frame = std::make_shared<PictureFrame>();
  ovt::render::Picture& pic = frame->picture;
  pic.coded_width = coded_width;
  pic.coded_height = coded_height;
  pic.color = color;
  pic.color.struct_size = sizeof(ovt_ColorDescription);
  pic.num_planes = color.chroma_format == 0 ? 1 : 3;
  const std::uint32_t sub_x = (color.chroma_format == 1 || color.chroma_format == 2) ? 2U : 1U;
  const std::uint32_t sub_y = color.chroma_format == 1 ? 2U : 1U;
  const auto sample_bytes = static_cast<std::uint8_t>(color.bit_depth <= 8 ? 1 : 2);
  for (std::size_t i = 0; i < pic.num_planes; ++i) {
    ovt::render::PlaneData& p = pic.planes[i];
    p.width = i == 0 ? coded_width : (coded_width + sub_x - 1) / sub_x;
    p.height = i == 0 ? coded_height : (coded_height + sub_y - 1) / sub_y;
    p.sample_bytes = sample_bytes;
    p.stride_bytes = p.width * sample_bytes;
    frame->planes.at(i).assign(static_cast<std::size_t>(p.stride_bytes) * p.height, 0);
    p.data = frame->planes.at(i).data();
  }
  pic.generation = next_picture_generation();
  return frame;
}

}  // namespace ovt::app

#endif  // OVT_APP_PICTURE_FRAME_H
