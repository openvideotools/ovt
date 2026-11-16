// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Registration helper of the performance harness (chapter 14 §14.9.1, WI-0.560).
//
// OVT_BENCHMARK(fn) registers fn with the settings of §14.9.1: milliseconds, MinTime(2.0), 5 repetitions, aggregates
// only (the median of the 5 is what tools/perf_compare.py reads). Google Benchmark lets a value registered in code
// win over its command-line flags, so tools/perf_run.py --min-time / --repetitions reach the binary through the
// environment variables OVT_PERF_MIN_TIME and OVT_PERF_REPETITIONS, which this header reads.

#ifndef OVT_TESTS_PERF_OVT_PERF_HPP
#define OVT_TESTS_PERF_OVT_PERF_HPP

#include <benchmark/benchmark.h>

#include <cstdlib>

namespace ovt::perf {

// Positive value of an environment variable, or `fallback` when it is unset, malformed or not positive.
inline double env_positive(const char* name, double fallback) {
  const char* text = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read once, before any thread starts
  if (text == nullptr) {
    return fallback;
  }
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  return (end != text && *end == '\0' && value > 0.0) ? value : fallback;
}

inline double min_time_seconds() {
  return env_positive("OVT_PERF_MIN_TIME", 2.0);
}
inline int repetitions() {
  return static_cast<int>(env_positive("OVT_PERF_REPETITIONS", 5.0));
}

}  // namespace ovt::perf

#define OVT_BENCHMARK(fn)                        \
  BENCHMARK(fn)                                  \
      ->Unit(benchmark::kMillisecond)            \
      ->MinTime(::ovt::perf::min_time_seconds()) \
      ->Repetitions(::ovt::perf::repetitions())  \
      ->ReportAggregatesOnly(true)

#endif  // OVT_TESTS_PERF_OVT_PERF_HPP
