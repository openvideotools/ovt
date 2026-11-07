// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SeriesAggregator (chapter 11 §11.3.1 "Performance"; WI-1.455a): the per-series level-of-detail tables behind the
// Timeline chart. When more than one frame falls on a device pixel, the pixel column shows the maximum of its frames
// (bars) and their mean (line). Both are answered from tables built once per series (O(n)): a maximum pyramid and
// prefix sums, so a column costs O(log n) and a whole chart O(width log n), however many frames there are.
//
// A value of NaN is a gap (a metric missing for some frames): it never contributes to a maximum or a mean, and a
// column with no value at all reports NaN. No Qt types: the aggregator is unit-tested without an application object.

#ifndef OVT_APP_MODELS_SERIES_AGGREGATOR_H
#define OVT_APP_MODELS_SERIES_AGGREGATOR_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ovt::app {

// One device-pixel column of the chart: the frames [first, last) (positions in the series) and their aggregates.
struct SeriesColumn {
  std::size_t first = 0;
  std::size_t last = 0;
  double max = 0.0;   // NaN when every frame of the column is a gap
  double mean = 0.0;  // NaN likewise
};

class SeriesAggregator {
 public:
  SeriesAggregator() = default;
  // Builds the tables; the values are kept (see value()).
  explicit SeriesAggregator(std::vector<double> values);

  [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
  [[nodiscard]] std::span<const double> values() const noexcept { return values_; }

  // Maximum / mean of the values in [first, last) (clamped to the series); NaN when the range holds no value.
  [[nodiscard]] double max(std::size_t first, std::size_t last) const;
  [[nodiscard]] double mean(std::size_t first, std::size_t last) const;

  // The columns of the frames [first, last) drawn `width` pixels wide. Column i covers
  // [first + i*n/width, first + (i+1)*n/width) with n = last - first, widened to one frame when zoomed in beyond one
  // pixel per frame (then neighbouring columns share a frame). Empty when n or width is 0.
  [[nodiscard]] std::vector<SeriesColumn> columns(std::size_t first, std::size_t last, std::size_t width) const;

 private:
  std::vector<double> values_;
  std::vector<std::vector<double>> levels_;  // levels_[L][i] = max of values_[i << L, (i + 1) << L); gaps are -inf
  std::vector<double> prefix_sum_;           // size n + 1, gaps count as 0
  std::vector<std::uint32_t> prefix_count_;  // size n + 1, number of non-gap values
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_SERIES_AGGREGATOR_H
