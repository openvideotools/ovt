// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Slot B of the compare modes: conversion into A's coded grid (chapter 10 §10.6.4 "Mismatch states", WI-1.410 step 1).
// The conversion runs on the CPU at set_compare() time: the caller's buffers are only valid during the call, and the
// difference shader then compares code values texel for texel.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pipeline/compare.h"

namespace ovt::render::detail {
namespace {

uint32_t sub_x(uint8_t chroma_format) {
  return (chroma_format == 1 || chroma_format == 2) ? 2 : 1;
}
uint32_t sub_y(uint8_t chroma_format) {
  return chroma_format == 1 ? 2 : 1;
}

const char* chroma_name(uint8_t chroma_format) {
  static constexpr std::array<const char*, 4> kNames = {"4:0:0", "4:2:0", "4:2:2", "4:4:4"};
  return chroma_format < kNames.size() ? kNames.at(chroma_format) : "?";
}

uint32_t read_sample(const PlaneData& p, uint32_t x, uint32_t y) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): row y of the caller's plane
  const auto* row = static_cast<const uint8_t*>(p.data) + (static_cast<size_t>(y) * p.stride_bytes);
  if (p.sample_bytes == 1) {
    return row[x];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  const size_t at = static_cast<size_t>(x) * 2;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): little-endian, LSB-aligned (§10.2.1)
  return static_cast<uint32_t>(row[at]) | (static_cast<uint32_t>(row[at + 1]) << 8U);
}

// B's sample value at the continuous plane position (px, py), nearest or bilinear, in B's bit depth.
double sample_b(const PlaneData& p, double px, double py, bool bilinear) {
  const auto max_x = static_cast<double>(p.width - 1);
  const auto max_y = static_cast<double>(p.height - 1);
  if (!bilinear) {
    return read_sample(p, static_cast<uint32_t>(std::clamp(std::floor(px), 0.0, max_x)),
                       static_cast<uint32_t>(std::clamp(std::floor(py), 0.0, max_y)));
  }
  const double cx = std::clamp(px, 0.0, max_x);
  const double cy = std::clamp(py, 0.0, max_y);
  const auto x0 = static_cast<uint32_t>(std::floor(cx));
  const auto y0 = static_cast<uint32_t>(std::floor(cy));
  const uint32_t x1 = std::min(x0 + 1, p.width - 1);
  const uint32_t y1 = std::min(y0 + 1, p.height - 1);
  const double fx = cx - x0;
  const double fy = cy - y0;
  const double top = (read_sample(p, x0, y0) * (1.0 - fx)) + (read_sample(p, x1, y0) * fx);
  const double bottom = (read_sample(p, x0, y1) * (1.0 - fx)) + (read_sample(p, x1, y1) * fx);
  return (top * (1.0 - fy)) + (bottom * fy);
}

void write_sample(std::vector<uint8_t>& buf, size_t index, uint8_t sample_bytes, uint32_t v) {
  if (sample_bytes == 1) {
    buf[index] = static_cast<uint8_t>(v);
    return;
  }
  buf[index * 2] = static_cast<uint8_t>(v & 0xFFU);
  buf[(index * 2) + 1] = static_cast<uint8_t>(v >> 8U);
}

// Fills plane `i` of the converted B in A's grid (`a`): chroma nearest (same size) or bilinear (allow_scale_b), depth
// by shift, neutral chroma when B has no chroma planes.
void convert_plane(const Picture& b, const SlotFormat& a, uint8_t i, PlaneData& p, std::vector<uint8_t>& buf) {
  const bool same_size = b.coded_width == a.width && b.coded_height == a.height;
  const uint32_t sx_a = i == 0 ? 1 : sub_x(a.chroma_format);
  const uint32_t sy_a = i == 0 ? 1 : sub_y(a.chroma_format);
  const uint32_t sx_b = i == 0 ? 1 : sub_x(b.color.chroma_format);
  const uint32_t sy_b = i == 0 ? 1 : sub_y(b.color.chroma_format);
  p.width = (a.width + sx_a - 1) / sx_a;
  p.height = (a.height + sy_a - 1) / sy_a;
  p.sample_bytes = a.sample_bytes;
  p.stride_bytes = p.width * a.sample_bytes;
  buf.assign(static_cast<size_t>(p.stride_bytes) * p.height, 0);
  p.data = buf.data();
  const double scale_x = static_cast<double>(b.coded_width) / a.width;
  const double scale_y = static_cast<double>(b.coded_height) / a.height;
  const double depth_scale = std::ldexp(1.0, static_cast<int>(a.bit_depth) - static_cast<int>(b.color.bit_depth));
  const auto max_code = static_cast<double>((1U << a.bit_depth) - 1U);
  const uint32_t neutral = 1U << (a.bit_depth - 1U);
  for (uint32_t y = 0; y < p.height; ++y) {
    for (uint32_t x = 0; x < p.width; ++x) {
      uint32_t v = neutral;
      if (i < b.num_planes) {
        double s = 0;
        if (same_size) {  // chroma: nearest, co-sited with the luma sample of A's chroma sample
          const uint32_t bx = (x * sx_a) / sx_b;
          const uint32_t by = (y * sy_a) / sy_b;
          s = sample_b(b.planes[i], static_cast<double>(bx), static_cast<double>(by), false);
        } else {  // allow_scale_b: bilinear at the sample centre (§10.2.4)
          const double px = ((((x + 0.5) * sx_a) * scale_x) / sx_b) - 0.5;
          const double py = ((((y + 0.5) * sy_a) * scale_y) / sy_b) - 0.5;
          s = sample_b(b.planes[i], px, py, true);
        }
        // Depth: shift (§10.6.4); a resampled value is rounded after the shift.
        v = static_cast<uint32_t>(std::min(std::floor((s * depth_scale) + (same_size ? 0.0 : 0.5)), max_code));
      }
      write_sample(buf, (static_cast<size_t>(y) * p.width) + x, a.sample_bytes, v);
    }
  }
}

}  // namespace

SlotFormat slot_format(const FrameSlot& slot) {
  SlotFormat f;
  f.valid = slot.valid;
  f.width = slot.coded_width;
  f.height = slot.coded_height;
  f.chroma_format = slot.chroma_format;
  f.bit_depth = slot.bit_depth;
  f.sample_bytes = slot.frame.repr.bits.sample_depth > 8 ? 2 : 1;
  f.crop = slot.crop;
  return f;
}

std::string prepare_b(const Picture& b, const SlotFormat& a, bool allow_scale_b, PreparedB& out) {
  out = PreparedB{};
  out.pic = b;
  std::string err = validate_picture(b);
  if (!err.empty() || !a.valid) {
    return err;
  }
  const bool same_size = b.coded_width == a.width && b.coded_height == a.height;
  if (!same_size && !allow_scale_b) {
    out.size_mismatch = true;
    out.message = "RND-0010 compare slot B is " + std::to_string(b.coded_width) + "×" + std::to_string(b.coded_height) +
                  ", A is " + std::to_string(a.width) + "×" + std::to_string(a.height) +
                  "; set allow_scale_b to resample";
    return "";
  }
  const uint8_t bd_b = b.color.bit_depth;
  const bool same_format =
      b.color.chroma_format == a.chroma_format && bd_b == a.bit_depth && b.planes[0].sample_bytes == a.sample_bytes;
  if (same_size && same_format) {
    return "";
  }

  // The §10.10 RND-0011 line names what changed; a pure resample (allow_scale_b) has no code of its own.
  std::vector<std::string> what;
  if (b.color.chroma_format != a.chroma_format) {
    what.push_back(std::string(chroma_name(b.color.chroma_format)) + "→" + chroma_name(a.chroma_format));
  }
  if (bd_b != a.bit_depth) {
    what.push_back(std::to_string(bd_b) + "-bit→" + std::to_string(a.bit_depth) + "-bit");
  }
  if (!what.empty()) {
    out.message = "RND-0011 B converted: " + what[0] + (what.size() > 1 ? ", " + what[1] : "");
  }
  out.converted = true;

  Picture& pic = out.pic;
  pic.coded_width = a.width;
  pic.coded_height = a.height;
  pic.crop = same_size ? b.crop : a.crop;
  pic.color.chroma_format = a.chroma_format;
  pic.color.bit_depth = a.bit_depth;
  pic.num_planes = a.chroma_format == 0 ? 1 : 3;
  out.storage.resize(pic.num_planes);
  for (uint8_t i = 0; i < pic.num_planes; ++i) {
    convert_plane(b, a, i, pic.planes[i], out.storage[i]);
  }
  return "";
}

}  // namespace ovt::render::detail
