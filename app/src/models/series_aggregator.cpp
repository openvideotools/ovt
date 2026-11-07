// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/series_aggregator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ovt::app {

namespace {
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

SeriesAggregator::SeriesAggregator(std::vector<double> values) : values_(std::move(values)) {
  const std::size_t n = values_.size();
  prefix_sum_.assign(n + 1, 0.0);
  prefix_count_.assign(n + 1, 0U);
  std::vector<double> base(n);
  for (std::size_t i = 0; i < n; ++i) {
    const bool gap = std::isnan(values_[i]);
    prefix_sum_[i + 1] = prefix_sum_[i] + (gap ? 0.0 : values_[i]);
    prefix_count_[i + 1] = prefix_count_[i] + (gap ? 0U : 1U);
    base[i] = gap ? kNegInf : values_[i];
  }
  levels_.push_back(std::move(base));
  while (levels_.back().size() > 1) {
    const std::vector<double>& below = levels_.back();
    std::vector<double> above((below.size() + 1) / 2);
    for (std::size_t i = 0; i < above.size(); ++i) {
      above[i] = std::max(below[2 * i], 2 * i + 1 < below.size() ? below[2 * i + 1] : kNegInf);
    }
    levels_.push_back(std::move(above));
  }
}

double SeriesAggregator::max(std::size_t first, std::size_t last) const {
  last = std::min(last, values_.size());
  if (first >= last) {
    return kNaN;
  }
  // The classic bottom-up range query: at each level take the odd boundary nodes, then halve both ends. A node of a
  // level is a complete block, or the (shorter) tail block of the series, which is inside [first, last) whenever its
  // parent range is.
  double best = kNegInf;
  std::size_t l = first;
  std::size_t r = last;
  for (std::size_t level = 0; l < r && level < levels_.size(); ++level) {
    if ((l & 1U) != 0) {
      best = std::max(best, levels_[level][l++]);
    }
    if ((r & 1U) != 0) {
      best = std::max(best, levels_[level][--r]);
    }
    l >>= 1U;
    r >>= 1U;
  }
  return best == kNegInf ? kNaN : best;
}

double SeriesAggregator::mean(std::size_t first, std::size_t last) const {
  last = std::min(last, values_.size());
  if (first >= last) {
    return kNaN;
  }
  const std::uint32_t count = prefix_count_[last] - prefix_count_[first];
  return count == 0 ? kNaN : (prefix_sum_[last] - prefix_sum_[first]) / static_cast<double>(count);
}

std::vector<SeriesColumn> SeriesAggregator::columns(std::size_t first, std::size_t last, std::size_t width) const {
  last = std::min(last, values_.size());
  std::vector<SeriesColumn> out;
  if (first >= last || width == 0) {
    return out;
  }
  // i * n reaches 4e9 for a 10^6-frame series across a 4 k chart, more than a 32-bit size_t holds (Android), so the
  // boundary is computed in double: both factors are exact, the quotient is at least 1/width away from any integer
  // it is not, so the floor is exact too.
  const auto n = static_cast<double>(last - first);
  const auto w = static_cast<double>(width);
  const auto boundary = [&](std::size_t i) { return first + static_cast<std::size_t>(static_cast<double>(i) * n / w); };
  out.reserve(width);
  for (std::size_t i = 0; i < width; ++i) {
    const std::size_t a = boundary(i);
    std::size_t b = boundary(i + 1);
    b = std::max(b, a + 1);
    out.push_back(SeriesColumn{a, b, max(a, b), mean(a, b)});
  }
  return out;
}

}  // namespace ovt::app
