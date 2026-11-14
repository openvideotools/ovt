// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vertex builders of the Timeline chart (chapter 11 §11.3.1; WI-1.455b): bars, the mean line, GOP ticks and the range
// band as plain coloured triangles / line segments. The layout of ColorVertex is that of
// QSGGeometry::ColoredPoint2D, so the chart item copies the vectors into its scene-graph nodes; keeping the
// arithmetic here makes it unit-testable without a scene graph.

#ifndef OVT_APP_MODELS_TIMELINE_GEOMETRY_H
#define OVT_APP_MODELS_TIMELINE_GEOMETRY_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "models/series_aggregator.h"
#include "ovt_model_generated.h"

namespace ovt::app {

struct Rgba {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;
  friend constexpr bool operator==(Rgba, Rgba) noexcept = default;
};

// Layout-compatible with QSGGeometry::ColoredPoint2D.
struct ColorVertex {
  float x = 0.0F;
  float y = 0.0F;
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 0;
};
static_assert(sizeof(ColorVertex) == 12);

// §11.4.5: I / Key / IntraOnly red, P / Inter blue, B green, Switch and unknown grey. A hidden frame (AV1
// `shown == false`) keeps its type colour; build_hatch() draws the hatch over it.
[[nodiscard]] Rgba frame_type_color(fb::FrameType type) noexcept;

// Two triangles per column: x spans the column's share of `width`, the height is value / max_value of `height`
// (zero or NaN: no bar). A gap of one device pixel separates bars once a column is wider than 3 px. `colors` has one
// entry per column (missing entries draw grey). Bars grow upward from y = height.
[[nodiscard]] std::vector<ColorVertex> build_bars(std::span<const SeriesColumn> columns, std::span<const Rgba> colors,
                                                  double max_value, float width, float height);

// Line segments (two vertices each) joining the means of neighbouring non-gap columns, scaled like the bars.
[[nodiscard]] std::vector<ColorVertex> build_mean_line(std::span<const SeriesColumn> columns, double max_value,
                                                       float width, float height, Rgba color);

// Diagonal hatch lines (line pairs, 45 degrees rising to the right, `spacing` px apart) clipped to the rectangle:
// the mark of a hidden frame (§11.4.5).
[[nodiscard]] std::vector<ColorVertex> build_hatch(float x0, float y0, float x1, float y1, Rgba color,
                                                   float spacing = 4.0F);

// One reference arrow of the chart: an arc between two x positions (the referencing frame and the frame it refers to).
struct RefArc {
  float x_from = 0.0F;
  float x_to = 0.0F;
  Rgba color;
};
// Arcs rising from y = `baseline` (line pairs of a 16-segment parabola); the height is half the horizontal distance,
// at most `max_height`.
[[nodiscard]] std::vector<ColorVertex> build_ref_arcs(std::span<const RefArc> arcs, float baseline, float max_height);

// A GOP tick at each x: a 2 px wide bar `tick_height` tall at the bottom of a chart `height` tall.
[[nodiscard]] std::vector<ColorVertex> build_ticks(std::span<const float> xs, float height, float tick_height,
                                                   Rgba color);

// A solid rectangle (two triangles).
[[nodiscard]] std::vector<ColorVertex> build_rect(float x0, float y0, float x1, float y1, Rgba color);

// The largest max over the columns, or 0 for none / all gaps: the top of the y axis.
[[nodiscard]] double max_of(std::span<const SeriesColumn> columns) noexcept;

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_TIMELINE_GEOMETRY_H
