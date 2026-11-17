// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// bench_index_h264_1h_1080p: ovt_stream_build_index over the one-hour 1080p H.264 stream of recipe 27
// (`syn_h264_1h_1080p`, chapter 14 §14.3.3; `tools/make_long_stream.sh` produces it), the budget of chapter 14
// §14.9.3 (≤ 2 000 ms on the M1 MacBook Air, CI fails above 2 500 ms) and the memory target of chapter 7 §7.4.5
// (≤ 200 MB RSS), WI-1.125 acceptance test 1. The counter `peak_rss_mb` is the process high-water mark after the
// first build in the process: what one `ovt index` run costs. `peak_rss_all_mb` is the high-water mark after
// every repetition; it is larger where the allocator keeps a worker thread's freed memory for that thread
// (glibc gives each pool worker its own arena, so repeats over different workers add up). The harness repeats
// the run, so the timings are warm-cache ones (the 2 s target is a warm-cache figure); drop the page cache before
// `tools/perf_run.py` for a cold measurement. Without the corpus file the benchmark is skipped with an error,
// never faked with a stand-in: the budget is about this stream.

#include <benchmark/benchmark.h>
#include <sys/resource.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "ovt/container.h"
#include "ovt/index.h"
#include "ovt/io.h"
#include "ovt/status.h"
#include "ovt_perf.hpp"

namespace {

namespace fs = std::filesystem;

// The corpus file in the cache layout of tools/fetch_corpora.py and the synthetic generator
// (<cache>/syn_h264_1h_1080p/<file>), or placed directly in $OVT_CORPORA_DIR.
std::string input_path() {
  const char* dir = std::getenv("OVT_CORPORA_DIR");  // NOLINT(concurrency-mt-unsafe): read once, before any thread
  if (dir == nullptr) {
    return {};
  }
  for (const char* name : {"syn_h264_1h_1080p.264", "syn_h264_1h_1080p.mp4"}) {
    for (const fs::path& p : {fs::path(dir) / "syn_h264_1h_1080p" / name, fs::path(dir) / name}) {
      if (fs::exists(p)) {
        return p.string();
      }
    }
  }
  return {};
}

double peak_rss_mb() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0.0;
  }
#if defined(__APPLE__)
  return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);  // bytes
#else
  return static_cast<double>(usage.ru_maxrss) / 1024.0;  // KiB
#endif
}

void bench_index_h264_1h_1080p(benchmark::State& state) {
  static const std::string path = input_path();
  if (path.empty()) {
    state.SkipWithError("syn_h264_1h_1080p.264 is not in $OVT_CORPORA_DIR (tools/make_long_stream.sh makes it)");
    return;
  }
  ovt_File* file = nullptr;
  ovt_Container* container = nullptr;
  ovt_Stream* stream = nullptr;
  if (ovt_file_open(path.c_str(), &file) != OVT_OK || ovt_container_open(file, &container) != OVT_OK ||
      ovt_stream_open(container, 0, &stream) != OVT_OK) {
    state.SkipWithError("cannot open the input as an Annex B H.264 stream");
    ovt_stream_close(stream);
    ovt_container_close(container);
    ovt_file_close(file);
    return;
  }
  static double first_build_rss_mb = 0.0;  // after the first build of the process, whichever repetition
  double index_bytes = 0;
  for (auto _ : state) {  // NOLINT(clang-analyzer-deadcode.DeadStores): the idiom of Google Benchmark
    ovt_Buffer index{nullptr, 0};
    if (ovt_stream_build_index(stream, nullptr, &index) != OVT_OK) {
      state.SkipWithError("ovt_stream_build_index failed");
      break;
    }
    if (first_build_rss_mb == 0.0) {
      first_build_rss_mb = peak_rss_mb();
    }
    benchmark::DoNotOptimize(index.size);
    index_bytes = static_cast<double>(index.size);
    // The module objects are linked without ovt_buffer_free() (core/src/status.cpp): a caller-owned buffer is a
    // new[] array (core/src/api/buffer.cpp), released here the way ovt_buffer_free() does.
    delete[] index.data;  // NOLINT(cppcoreguidelines-owning-memory)
  }
  state.counters["index_bytes"] = index_bytes;
  state.counters["peak_rss_mb"] = first_build_rss_mb;
  state.counters["peak_rss_all_mb"] = peak_rss_mb();
  ovt_stream_close(stream);
  ovt_container_close(container);
  ovt_file_close(file);
}

}  // namespace

OVT_BENCHMARK(bench_index_h264_1h_1080p);

BENCHMARK_MAIN();
