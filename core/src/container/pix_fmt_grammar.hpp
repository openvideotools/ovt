// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The pix_fmt string grammar of headerless raw video (chapter 08 §8.6.6, WI-1.229): FFmpeg pixel-format names,
// the layout each one means and the frame size in bytes.

#ifndef OVT_CONTAINER_PIX_FMT_GRAMMAR_HPP
#define OVT_CONTAINER_PIX_FMT_GRAMMAR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ovt::container::raw {

inline constexpr uint32_t kMaxDimension = 1U << 20U;  // per axis; keeps every frame_bytes far below 2^64

enum class PixLayout : uint8_t {
  Planar,      // yuvXYZp[bits][le|be]: Y, U, V planes
  SemiPlanar,  // nv12 nv21 nv16 nv24 p010 p012 p016 p210 p410: Y plane then one interleaved chroma plane
  Gray,        // gray[bits][le|be]: one plane
  PackedRgb,   // rgb24 bgr24 rgb48le/be: one interleaved plane
  PlanarRgb,   // gbrp[bits][le|be]: three full-size planes
};

// One parsed pix_fmt name.
struct PixFmt {
  std::string name;  // exactly the accepted spelling, e.g. "yuv420p10le"
  PixLayout layout = PixLayout::Planar;
  // ovt_ColorDescription.chroma_format: 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4, 4 = 4:1:1.
  // SPEC-ASSUMPTION(#366): 4:1:1 is 4 (as in the Y4M reader); 4:4:0 and RGB have no value of their own and
  // map to 2 (vertical-only subsampling is the 4:2:2 transpose) and 3.
  uint8_t chroma_format = 1;
  uint8_t bit_depth = 8;
  uint8_t sub_x = 2;  // horizontal chroma subsampling factor (0 = no chroma plane)
  uint8_t sub_y = 2;  // vertical chroma subsampling factor (0 = no chroma plane)
  uint8_t bytes_per_sample = 1;
  bool little_endian = true;
  bool planar = true;           // false for packed RGB
  bool interleaved_uv = false;  // nv12 / p010 family
};

// nullopt when `name` is not in the grammar. The whole string must match; there is no trimming or case folding.
[[nodiscard]] std::optional<PixFmt> parse_pix_fmt(std::string_view name);

// Bytes of one frame (Σ planes of plane_w × plane_h × bytes_per_sample; chroma dimensions round up). Zero for a
// zero or oversized dimension.
[[nodiscard]] uint64_t frame_bytes(const PixFmt& fmt, uint32_t width, uint32_t height) noexcept;

}  // namespace ovt::container::raw

#endif  // OVT_CONTAINER_PIX_FMT_GRAMMAR_HPP
