// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of SeriesAggregator (WI-1.455a; chapter 11 §11.3.1). Acceptance test 1: given a 10^6-frame synthetic index,
// zoomed to fit, the aggregation takes at most 16 ms and every bar equals the per-pixel maximum.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "models/series_aggregator.h"

namespace {

using ovt::app::SeriesAggregator;
using ovt::app::SeriesColumn;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A deterministic xorshift stream: bits-like values with the occasional huge I frame.
std::vector<double> synthetic(std::size_t n, std::uint64_t seed) {
  std::vector<double> out(n);
  std::uint64_t x = seed;
  for (double& v : out) {
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    v = static_cast<double>(x % 4000U) + ((x % 61U) == 0U ? 200000.0 : 0.0);
  }
  return out;
}

double brute_max(const std::vector<double>& v, std::size_t a, std::size_t b) {
  double best = -std::numeric_limits<double>::infinity();
  for (std::size_t i = a; i < b; ++i) {
    if (!std::isnan(v[i])) {
      best = std::max(best, v[i]);
    }
  }
  return std::isinf(best) ? kNaN : best;
}

double brute_mean(const std::vector<double>& v, std::size_t a, std::size_t b) {
  double sum = 0.0;
  std::size_t n = 0;
  for (std::size_t i = a; i < b; ++i) {
    if (!std::isnan(v[i])) {
      sum += v[i];
      ++n;
    }
  }
  return n == 0 ? kNaN : sum / static_cast<double>(n);
}

bool same(double a, double b) {
  return (std::isnan(a) && std::isnan(b)) || std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b));
}

}  // namespace

TEST_CASE("10^6 frames zoomed to fit: bars equal the per-pixel maxima within 16 ms", "[fast][gui][timeline][aggregator]") {
  constexpr std::size_t kFrames = 1'000'000;
  constexpr std::size_t kWidth = 1920;
  const std::vector<double> values = synthetic(kFrames, 0x9E3779B97F4A7C15ULL);
  const SeriesAggregator agg(values);  // the tables are built once, outside the paint

  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<SeriesColumn> columns = agg.columns(0, kFrames, kWidth);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(elapsed <= std::chrono::milliseconds(16));

  REQUIRE(columns.size() == kWidth);
  std::size_t expected_first = 0;
  for (const SeriesColumn& c : columns) {
    REQUIRE(c.first == expected_first);  // the columns tile the range
    REQUIRE(c.last > c.first);
    REQUIRE(c.max == brute_max(values, c.first, c.last));
    REQUIRE(same(c.mean, brute_mean(values, c.first, c.last)));
    expected_first = c.last;
  }
  CHECK(expected_first == kFrames);
}

TEST_CASE("max and mean of arbitrary ranges equal brute force", "[fast][gui][timeline][aggregator]") {
  for (const std::size_t n : {1U, 2U, 3U, 5U, 7U, 64U, 100U, 1023U, 1025U}) {
    const std::vector<double> values = synthetic(n, 12345U + n);
    const SeriesAggregator agg(values);
    for (std::size_t a = 0; a < n; a += (n > 40 ? 7 : 1)) {
      for (std::size_t b = a + 1; b <= n; b += (n > 40 ? 5 : 1)) {
        REQUIRE(agg.max(a, b) == brute_max(values, a, b));
        REQUIRE(same(agg.mean(a, b), brute_mean(values, a, b)));
      }
    }
  }
}

TEST_CASE("gaps (NaN) never contribute and an all-gap column is NaN", "[fast][gui][timeline][aggregator]") {
  const SeriesAggregator agg({kNaN, 4.0, kNaN, 2.0, kNaN, kNaN});
  CHECK(agg.max(0, 6) == 4.0);
  CHECK(agg.mean(0, 6) == 3.0);
  CHECK(std::isnan(agg.max(4, 6)));
  CHECK(std::isnan(agg.mean(4, 6)));
  CHECK(std::isnan(agg.max(2, 2)));  // empty range
  CHECK(agg.max(3, 100) == 2.0);     // clamped
  const std::vector<SeriesColumn> cols = agg.columns(0, 6, 3);
  REQUIRE(cols.size() == 3);
  CHECK(cols[0].max == 4.0);
  CHECK(cols[1].max == 2.0);
  CHECK(std::isnan(cols[2].max));
}

TEST_CASE("zoomed in beyond one pixel per frame, columns are one frame wide and share frames",
          "[fast][gui][timeline][aggregator]") {
  const SeriesAggregator agg({1.0, 5.0, 3.0});
  const std::vector<SeriesColumn> cols = agg.columns(0, 3, 9);
  REQUIRE(cols.size() == 9);
  for (std::size_t i = 0; i < cols.size(); ++i) {
    CHECK(cols[i].last == cols[i].first + 1);
    CHECK(cols[i].first == i / 3);
    CHECK(cols[i].max == agg.values()[i / 3]);
  }
  CHECK(agg.columns(0, 0, 10).empty());
  CHECK(agg.columns(0, 3, 0).empty());
  CHECK(SeriesAggregator().columns(0, 1, 4).empty());
}
