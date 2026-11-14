// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/timeline_geometry.h"

#include <algorithm>
#include <cmath>

namespace ovt::app {

namespace {

constexpr Rgba kGrey{0x9E, 0x9E, 0x9E, 255};

void push_rect(std::vector<ColorVertex>& out, float x0, float y0, float x1, float y1, Rgba c) {
  const ColorVertex tl{x0, y0, c.r, c.g, c.b, c.a};
  const ColorVertex tr{x1, y0, c.r, c.g, c.b, c.a};
  const ColorVertex bl{x0, y1, c.r, c.g, c.b, c.a};
  const ColorVertex br{x1, y1, c.r, c.g, c.b, c.a};
  out.insert(out.end(), {tl, tr, bl, tr, br, bl});
}

}  // namespace

Rgba frame_type_color(fb::FrameType type) noexcept {
  Rgba c = kGrey;
  switch (type) {
    case fb::FrameType::I:
    case fb::FrameType::Key:
    case fb::FrameType::IntraOnly:
      c = Rgba{0xE5, 0x39, 0x35, 255};
      break;
    case fb::FrameType::P:
    case fb::FrameType::Inter:
      c = Rgba{0x1E, 0x88, 0xE5, 255};
      break;
    case fb::FrameType::B:
      c = Rgba{0x43, 0xA0, 0x47, 255};
      break;
    case fb::FrameType::Switch:
    case fb::FrameType::Unknown:
      break;
  }
  return c;
}

double max_of(std::span<const SeriesColumn> columns) noexcept {
  double best = 0.0;
  for (const SeriesColumn& c : columns) {
    if (!std::isnan(c.max)) {
      best = std::max(best, c.max);
    }
  }
  return best;
}

std::vector<ColorVertex> build_bars(std::span<const SeriesColumn> columns, std::span<const Rgba> colors,
                                    double max_value, float width, float height) {
  std::vector<ColorVertex> out;
  if (columns.empty() || !(max_value > 0.0)) {
    return out;
  }
  out.reserve(columns.size() * 6);
  const float column_width = width / static_cast<float>(columns.size());
  const float gap = column_width > 3.0F ? 1.0F : 0.0F;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const double value = columns[i].max;
    if (std::isnan(value) || value <= 0.0) {
      continue;
    }
    const float x0 = static_cast<float>(i) * column_width;
    const float x1 = x0 + column_width - gap;
    const auto h = static_cast<float>(std::min(value / max_value, 1.0)) * height;
    push_rect(out, x0, height - h, x1, height, i < colors.size() ? colors[i] : kGrey);
  }
  return out;
}

std::vector<ColorVertex> build_mean_line(std::span<const SeriesColumn> columns, double max_value, float width,
                                         float height, Rgba color) {
  std::vector<ColorVertex> out;
  if (columns.size() < 2 || !(max_value > 0.0)) {
    return out;
  }
  const float column_width = width / static_cast<float>(columns.size());
  const auto point = [&](std::size_t i) {
    const auto h = static_cast<float>(std::clamp(columns[i].mean / max_value, 0.0, 1.0)) * height;
    return ColorVertex{(static_cast<float>(i) + 0.5F) * column_width, height - h, color.r, color.g, color.b, color.a};
  };
  for (std::size_t i = 1; i < columns.size(); ++i) {
    if (!std::isnan(columns[i - 1].mean) && !std::isnan(columns[i].mean)) {
      out.push_back(point(i - 1));
      out.push_back(point(i));
    }
  }
  return out;
}

std::vector<ColorVertex> build_hatch(float x0, float y0, float x1, float y1, Rgba color, float spacing) {
  std::vector<ColorVertex> out;
  const float rect_height = y1 - y0;
  if (!(rect_height > 0.0F) || !(x1 > x0) || !(spacing > 0.0F)) {
    return out;
  }
  // A line starts at (xa, y1) and rises one pixel per pixel to the right; clip it to the rectangle.
  for (int k = 0;; ++k) {
    const float xa = x0 - rect_height + static_cast<float>(k) * spacing;
    if (!(xa < x1)) {
      break;
    }
    const float t0 = std::max(0.0F, x0 - xa);
    const float t1 = std::min(rect_height, x1 - xa);
    if (t1 > t0) {
      out.push_back(ColorVertex{xa + t0, y1 - t0, color.r, color.g, color.b, color.a});
      out.push_back(ColorVertex{xa + t1, y1 - t1, color.r, color.g, color.b, color.a});
    }
  }
  return out;
}

std::vector<ColorVertex> build_ref_arcs(std::span<const RefArc> arcs, float baseline, float max_height) {
  constexpr int kSegments = 16;
  std::vector<ColorVertex> out;
  out.reserve(arcs.size() * kSegments * 2);
  for (const RefArc& arc : arcs) {
    const float height = std::min(max_height, 0.5F * std::abs(arc.x_to - arc.x_from));
    const auto point = [&](int i) {
      const float t = static_cast<float>(i) / static_cast<float>(kSegments);
      return ColorVertex{arc.x_from + (arc.x_to - arc.x_from) * t,
                         baseline - 4.0F * height * t * (1.0F - t),
                         arc.color.r,
                         arc.color.g,
                         arc.color.b,
                         arc.color.a};
    };
    for (int i = 0; i < kSegments; ++i) {
      out.push_back(point(i));
      out.push_back(point(i + 1));
    }
  }
  return out;
}

std::vector<ColorVertex> build_ticks(std::span<const float> xs, float height, float tick_height, Rgba color) {
  std::vector<ColorVertex> out;
  out.reserve(xs.size() * 6);
  for (const float x : xs) {
    push_rect(out, x - 1.0F, height - tick_height, x + 1.0F, height, color);
  }
  return out;
}

std::vector<ColorVertex> build_rect(float x0, float y0, float x1, float y1, Rgba color) {
  std::vector<ColorVertex> out;
  push_rect(out, x0, y0, x1, y1, color);
  return out;
}

}  // namespace ovt::app
