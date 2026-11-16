// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// bench_metrics: the metric rows of chapter 14 §14.9.3 (WI-3.590 adds the PSNR/SSIM rows):
//
//   bench_metrics_psnr_1080p       psnr, one thread                     counter fps_1_thread          (>= 600 on M1)
//   bench_metrics_psnr_ssim_1080p  psnr + ssim, four threads             counter fps_4_threads         (>= 200 on M1)
//   bench_metrics_maps             psnr + ssim with 16x16 maps vs none   counter overhead_ms_per_frame (<= 1.5 on M1)
//
// Input: the first 60 pictures of syn_metrics_1080p_pair (chapter 14 §14.3.3 recipe 25) from the corpus cache
// ($OVT_CORPORA_DIR, default build/corpora): syn_metrics_1080p_ref.y4m and the decoded distorted stream
// syn_metrics_1080p_dist.y4m. Until the fetcher writes the decoded file, a deterministic 1080p 8-bit 4:2:0 pair
// (gradient plus noise) of the same size stands in; PSNR and SSIM cost does not depend on the content, and the
// benchmark label says which input ran. The kernels are driven through their ovt_metric_v1 tables, the way the
// metric job calls them; four threads score disjoint frames with one instance pair each (frame parallelism, chapter 9
// §9.2.3), since MetricJob cannot decode yet.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "metrics/kernels/map_sink.h"
#include "metrics/kernels/psnr_metric.h"
#include "metrics/kernels/ssim_metric.h"
#include "ovt_perf.hpp"

namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr size_t kFrames = 60;
constexpr int kThreads = 4;

// One 8-bit 4:2:0 picture.
struct Picture {
  std::array<std::vector<uint8_t>, 3> buf;
  ovt_MetricPlanes planes{};
  Picture() {
    planes.struct_size = sizeof(ovt_MetricPlanes);
    planes.planes = 3;
    planes.bit_depth = 8;
    planes.sample_bytes = 1;
    planes.sample_format = OVT_METRIC_SAMPLE_UINT;
    planes.domain = OVT_METRIC_DOMAIN_CODE_VALUE;
    for (size_t p = 0; p < 3; ++p) {
      const uint32_t w = p == 0 ? kWidth : kWidth / 2;
      const uint32_t h = p == 0 ? kHeight : kHeight / 2;
      buf.at(p).assign(static_cast<size_t>(w) * h, 0);
      planes.data[p] = buf.at(p).data();
      planes.stride_bytes[p] = static_cast<int32_t>(w);
      planes.width[p] = w;
      planes.height[p] = h;
    }
  }
};

struct Pair {
  std::vector<std::unique_ptr<Picture>> ref;
  std::vector<std::unique_ptr<Picture>> dist;
  std::string label;
};

// The pictures of a 1920x1080 8-bit 4:2:0 Y4M file, at most kFrames; empty when the file is absent or differs.
std::vector<std::unique_ptr<Picture>> read_y4m(const std::filesystem::path& path) {
  std::vector<std::unique_ptr<Picture>> out;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line.find(" W1920 ") == std::string::npos ||
      line.find(" H1080") == std::string::npos || line.find("C420p1") != std::string::npos) {
    return out;
  }
  while (out.size() < kFrames && std::getline(in, line) && line.rfind("FRAME", 0) == 0) {
    auto pic = std::make_unique<Picture>();
    for (auto& plane : pic->buf) {
      in.read(reinterpret_cast<char*>(plane.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
              static_cast<std::streamsize>(plane.size()));
    }
    if (!in) {
      break;
    }
    out.push_back(std::move(pic));
  }
  return out;
}

// Gradient plus xorshift64 noise; `noise` = 0 for the reference.
std::unique_ptr<Picture> synthetic(size_t frame, uint32_t noise) {
  auto pic = std::make_unique<Picture>();
  uint64_t s = 0x9E3779B97F4A7C15ULL + frame;
  for (size_t p = 0; p < 3; ++p) {
    auto& b = pic->buf.at(p);
    const uint32_t w = pic->planes.width[p];
    for (size_t i = 0; i < b.size(); ++i) {
      s ^= s << 13U;
      s ^= s >> 7U;
      s ^= s << 17U;
      const auto x = static_cast<uint32_t>(i % w);
      const auto y = static_cast<uint32_t>(i / w);
      const uint32_t v = ((x + y + static_cast<uint32_t>(frame * 4)) & 0xFFU) + static_cast<uint32_t>(s % (noise + 1U));
      b[i] = static_cast<uint8_t>(std::min(v, 255U));
    }
  }
  return pic;
}

const Pair& pair() {
  static const Pair p = [] {
    Pair out;
    const char* env = std::getenv("OVT_CORPORA_DIR");  // NOLINT(concurrency-mt-unsafe): read before threads start
    const std::filesystem::path dir =
        std::filesystem::path(env != nullptr && *env != '\0' ? env : "build/corpora") / "syn_metrics_1080p_pair";
    out.ref = read_y4m(dir / "syn_metrics_1080p_ref.y4m");
    out.dist = read_y4m(dir / "syn_metrics_1080p_dist.y4m");
    if (!out.ref.empty() && out.ref.size() == out.dist.size()) {
      out.label = "syn_metrics_1080p_pair";
      return out;
    }
    out.ref.clear();
    out.dist.clear();
    for (size_t f = 0; f < kFrames; ++f) {
      out.ref.push_back(synthetic(f, 0));
      out.dist.push_back(synthetic(f, 24));
    }
    out.label = "synthetic 1080p (syn_metrics_1080p_pair not in the corpus cache)";
    return out;
  }();
  return p;
}

// One metric instance.
class Metric {
 public:
  Metric(const ovt_MetricApi* api, const char* options) : api_(api) {
    if (api_->create(options, &m_) != OVT_OK) {
      std::abort();
    }
  }
  Metric(const Metric&) = delete;
  Metric& operator=(const Metric&) = delete;
  Metric(Metric&&) = delete;
  Metric& operator=(Metric&&) = delete;
  ~Metric() { api_->destroy(m_); }
  void score(const Picture& r, const Picture& d, int32_t frame) const {
    ovt_Buffer out{};
    if (api_->score(m_, &r.planes, &d.planes, ovt_FrameId{frame, frame}, &out) != OVT_OK) {
      std::abort();
    }
    benchmark::DoNotOptimize(out.data);
  }

 private:
  const ovt_MetricApi* api_;
  ovt_Metric* m_ = nullptr;
};

// A sink that keeps nothing: the cost measured is computing and encoding the maps, not storing them.
class DiscardSink final : public ovt::metrics::MapSink {
 public:
  void put(const std::string& /*blob_id*/, std::vector<uint8_t> ovtmap) override { benchmark::DoNotOptimize(ovtmap); }
};

// psnr on one thread, every frame of the pair per iteration.
void bench_metrics_psnr_1080p(benchmark::State& state) {
  const Pair& in = pair();
  const Metric psnr(ovt::metrics::psnr_metric_api(), "{}");
  int64_t frames = 0;
  for (auto _ : state) {
    for (size_t f = 0; f < in.ref.size(); ++f) {
      psnr.score(*in.ref[f], *in.dist[f], static_cast<int32_t>(f));
    }
    frames += static_cast<int64_t>(in.ref.size());
  }
  state.counters["fps_1_thread"] = benchmark::Counter(static_cast<double>(frames), benchmark::Counter::kIsRate);
  state.SetLabel(in.label);
}
OVT_BENCHMARK(bench_metrics_psnr_1080p)->UseRealTime();

// psnr + ssim (defaults: all planes for psnr, luma Gaussian SSIM with libvmaf's border) on four threads, each scoring
// every fourth frame with its own instances.
void bench_metrics_psnr_ssim_1080p(benchmark::State& state) {
  const Pair& in = pair();
  int64_t frames = 0;
  for (auto _ : state) {
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&in, t] {
        const Metric psnr(ovt::metrics::psnr_metric_api(), "{}");
        const Metric ssim(ovt::metrics::ssim_metric_api(), "{}");
        for (size_t f = static_cast<size_t>(t); f < in.ref.size(); f += kThreads) {
          psnr.score(*in.ref[f], *in.dist[f], static_cast<int32_t>(f));
          ssim.score(*in.ref[f], *in.dist[f], static_cast<int32_t>(f));
        }
      });
    }
    for (auto& w : workers) {
      w.join();
    }
    frames += static_cast<int64_t>(in.ref.size());
  }
  state.counters["fps_4_threads"] = benchmark::Counter(static_cast<double>(frames), benchmark::Counter::kIsRate);
  state.SetLabel(in.label);
}
OVT_BENCHMARK(bench_metrics_psnr_ssim_1080p)->UseRealTime();

// The extra time per frame of psnr + ssim with map_block 16 (maps on Y for ssim, Y/U/V for psnr) over the same
// metrics without maps, one thread.
void bench_metrics_maps(benchmark::State& state) {
  const Pair& in = pair();
  DiscardSink sink;
  const Metric psnr(ovt::metrics::psnr_metric_api(), "{}");
  const Metric ssim(ovt::metrics::ssim_metric_api(), "{}");
  const Metric psnr_map(ovt::metrics::psnr_metric_api(), R"({"map_block":16})");
  const Metric ssim_map(ovt::metrics::ssim_metric_api(), R"({"map_block":16})");
  ovt::metrics::set_map_sink(&sink);
  double overhead_ms = 0.0;
  int64_t frames = 0;
  for (auto _ : state) {
    for (size_t f = 0; f < in.ref.size(); ++f) {
      const auto f32 = static_cast<int32_t>(f);
      const auto t0 = std::chrono::steady_clock::now();
      psnr.score(*in.ref[f], *in.dist[f], f32);
      ssim.score(*in.ref[f], *in.dist[f], f32);
      const auto t1 = std::chrono::steady_clock::now();
      psnr_map.score(*in.ref[f], *in.dist[f], f32);
      ssim_map.score(*in.ref[f], *in.dist[f], f32);
      const auto t2 = std::chrono::steady_clock::now();
      overhead_ms += std::chrono::duration<double, std::milli>((t2 - t1) - (t1 - t0)).count();
    }
    frames += static_cast<int64_t>(in.ref.size());
  }
  ovt::metrics::set_map_sink(nullptr);
  state.counters["overhead_ms_per_frame"] = frames > 0 ? overhead_ms / static_cast<double>(frames) : 0.0;
  state.SetLabel(in.label);
}
OVT_BENCHMARK(bench_metrics_maps)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
