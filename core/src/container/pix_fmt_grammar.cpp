// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The pix_fmt grammar of chapter 08 §8.6.6 (WI-1.229).

#include "container/pix_fmt_grammar.hpp"

#include <array>

namespace ovt::container::raw {

namespace {

struct Ending {
  uint8_t depth = 8;
  bool little_endian = true;
};

// "[bits](le|be)" after the family. `required`: the suffix must be present (bits imply an endian; an endian
// alone is not accepted). nullopt when `s` is not a valid ending.
std::optional<Ending> parse_ending(std::string_view s, bool allow_empty, bool endian_optional) {
  Ending e;
  if (s.empty()) {
    return allow_empty ? std::optional<Ending>(e) : std::nullopt;
  }
  uint8_t depth = 0;
  for (const std::string_view d : {"16", "14", "12", "10", "9"}) {
    if (s.starts_with(d)) {
      depth = static_cast<uint8_t>(d == "9" ? 9 : (d[0] - '0') * 10 + (d[1] - '0'));
      s.remove_prefix(d.size());
      break;
    }
  }
  if (depth == 0) {
    return std::nullopt;
  }
  e.depth = depth;
  if (s.empty()) {
    return endian_optional ? std::optional<Ending>(e) : std::nullopt;  // bits need an endian (planar, gray, gbrp)
  }
  if (s == "le") {
    e.little_endian = true;
  } else if (s == "be") {
    e.little_endian = false;
  } else {
    return std::nullopt;
  }
  return e;
}

uint8_t chroma_format_of(uint8_t sx, uint8_t sy) {
  if (sx == 0) {
    return 0;
  }
  if (sx == 2 && sy == 2) {
    return 1;
  }
  if (sx == 1 && sy == 1) {
    return 3;
  }
  if (sx == 4 && sy == 1) {
    return 4;
  }
  return 2;  // 4:2:2 and 4:4:0
}

std::optional<PixFmt> planar_yuv(std::string_view name) {
  struct Family {
    std::string_view digits;
    uint8_t sx;
    uint8_t sy;
  };
  static constexpr std::array<Family, 6> kFamilies = {{
      {"400", 0, 0},
      {"420", 2, 2},
      {"422", 2, 1},
      {"444", 1, 1},
      {"411", 4, 1},
      {"440", 1, 2},
  }};
  if (!name.starts_with("yuv") || name.size() < 7) {
    return std::nullopt;
  }
  const std::string_view digits = name.substr(3, 3);
  for (const Family& f : kFamilies) {
    if (f.digits != digits || name[6] != 'p') {
      continue;
    }
    const auto end = parse_ending(name.substr(7), true, false);
    if (!end) {
      return std::nullopt;
    }
    PixFmt p;
    p.name = std::string(name);
    p.layout = PixLayout::Planar;
    p.sub_x = f.sx;
    p.sub_y = f.sy;
    p.chroma_format = chroma_format_of(f.sx, f.sy);
    p.bit_depth = end->depth;
    p.bytes_per_sample = end->depth > 8 ? 2 : 1;
    p.little_endian = end->little_endian;
    return p;
  }
  return std::nullopt;
}

std::optional<PixFmt> semi_planar(std::string_view name) {
  struct Row {
    std::string_view base;
    uint8_t sx;
    uint8_t sy;
    uint8_t depth;
    bool wide;  // p0xx: 16-bit samples with an optional endian suffix
  };
  static constexpr std::array<Row, 9> kRows = {{
      {"nv12", 2, 2, 8, false},
      {"nv21", 2, 2, 8, false},
      {"nv16", 2, 1, 8, false},
      {"nv24", 1, 1, 8, false},
      {"p010", 2, 2, 10, true},
      {"p012", 2, 2, 12, true},
      {"p016", 2, 2, 16, true},
      {"p210", 2, 1, 10, true},
      {"p410", 1, 1, 10, true},
  }};
  for (const Row& r : kRows) {
    if (!name.starts_with(r.base)) {
      continue;
    }
    const std::string_view rest = name.substr(r.base.size());
    PixFmt p;
    p.name = std::string(name);
    p.layout = PixLayout::SemiPlanar;
    p.sub_x = r.sx;
    p.sub_y = r.sy;
    p.chroma_format = chroma_format_of(r.sx, r.sy);
    p.bit_depth = r.depth;
    p.bytes_per_sample = r.wide ? 2 : 1;
    p.interleaved_uv = true;
    if (rest.empty()) {
      p.little_endian = true;  // §8.6.6: a bare p010 is little-endian
      return p;
    }
    if (!r.wide || (rest != "le" && rest != "be")) {
      return std::nullopt;
    }
    p.little_endian = rest == "le";
    return p;
  }
  return std::nullopt;
}

std::optional<PixFmt> gray(std::string_view name) {
  if (!name.starts_with("gray")) {
    return std::nullopt;
  }
  const auto end = parse_ending(name.substr(4), true, false);
  if (!end) {
    return std::nullopt;
  }
  PixFmt p;
  p.name = std::string(name);
  p.layout = PixLayout::Gray;
  p.sub_x = 0;
  p.sub_y = 0;
  p.chroma_format = 0;
  p.bit_depth = end->depth;
  p.bytes_per_sample = end->depth > 8 ? 2 : 1;
  p.little_endian = end->little_endian;
  return p;
}

std::optional<PixFmt> rgb(std::string_view name) {
  PixFmt p;
  p.name = std::string(name);
  p.sub_x = 1;
  p.sub_y = 1;
  p.chroma_format = 3;  // §8.6.6: RGB is reported as 4:4:4
  if (name == "rgb24" || name == "bgr24") {
    p.layout = PixLayout::PackedRgb;
    p.planar = false;
    return p;
  }
  if (name == "rgb48le" || name == "rgb48be") {
    p.layout = PixLayout::PackedRgb;
    p.planar = false;
    p.bit_depth = 16;
    p.bytes_per_sample = 2;
    p.little_endian = name == "rgb48le";
    return p;
  }
  if (name.starts_with("gbrp")) {
    const auto end = parse_ending(name.substr(4), true, false);
    if (!end) {
      return std::nullopt;
    }
    p.layout = PixLayout::PlanarRgb;
    p.bit_depth = end->depth;
    p.bytes_per_sample = end->depth > 8 ? 2 : 1;
    p.little_endian = end->little_endian;
    return p;
  }
  return std::nullopt;
}

uint64_t ceil_div(uint64_t a, uint64_t b) {
  return (a + b - 1) / b;
}

}  // namespace

std::optional<PixFmt> parse_pix_fmt(std::string_view name) {
  if (name.empty() || name.size() > 16) {
    return std::nullopt;
  }
  if (auto p = planar_yuv(name)) {
    return p;
  }
  if (auto p = semi_planar(name)) {
    return p;
  }
  if (auto p = gray(name)) {
    return p;
  }
  return rgb(name);
}

uint64_t frame_bytes(const PixFmt& fmt, uint32_t width, uint32_t height) noexcept {
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
    return 0;
  }
  const uint64_t w = width;
  const uint64_t h = height;
  const uint64_t bps = fmt.bytes_per_sample;
  switch (fmt.layout) {
    case PixLayout::PackedRgb:
    case PixLayout::PlanarRgb:
      return w * h * 3 * bps;
    case PixLayout::Gray:
      return w * h * bps;
    case PixLayout::Planar:
    case PixLayout::SemiPlanar:
      break;
  }
  if (fmt.sub_x == 0) {  // yuv400p: no chroma planes
    return w * h * bps;
  }
  // Both layouts: bps × (w·h + 2·cw·ch); in the semi-planar layout the 2·cw·ch chroma samples are interleaved
  // in one plane.
  return bps * (w * h + 2 * ceil_div(w, fmt.sub_x) * ceil_div(h, fmt.sub_y));
}

}  // namespace ovt::container::raw
