// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// bench_render_video: the video-path render rows of chapter 14 §14.9.3 (chapter 10 §10.9.3, WI-1.415):
//
//   bench_render_video_4k      VideoSurface::render() of a 4K 10-bit PQ frame to an SDR offscreen target (tone map
//                              BT.2390), no overlays, along a scripted zoom/pan path   counters p50_ms p95_ms p99_ms
//                              (budget on m1-air-8gb: p95_ms <= 8)
//   bench_render_first_frame   VideoSurface creation + init + set_frame + first render()   counters cold_ms warm_ms
//                              (budget on m1-air-8gb: warm_ms <= 150, cold_ms <= 1500)
//
// Flags besides those of Google Benchmark (§14.9.1; tools/perf_run.py passes only the Google Benchmark ones):
//
//   --fixture <fb>     a FrameAnalysis FlatBuffer (render/tests/fixtures/real_4k_hevc_frame.fb or
//                      real_1080p_hevc_frame.fb, chapter 14 §14.3.3). Its coded size is the picture size; the planes
//                      are always synthetic, because the fixture carries analysis only, never samples. A missing or
//                      invalid file is an error (exit 2), not a silent fallback. Default: 3840x2160 for the 4K
//                      benchmark, 1920x1080 for the first-frame one.
//   --frames N         length of the scripted path of bench_render_video_4k (default 100, the "100 frames" of §14.9.3)
//   --viewport WxH     size of the offscreen target (default: the size of the picture)
//
// Input (§14.9.3, #627): only the coded size of the fixture is used; the planes are a synthetic picture, because a
// FrameAnalysis carries no samples.
//
// Times are CPU wall-clock times of the calling thread around the call. VideoSurface::render() returns only after the
// GPU has finished an offscreen frame (the surface waits on its own timeline semaphore before it hands the texture
// back), so the wall time includes the GPU. libovt-render deliberately hides Vulkan timestamp queries (#304,
// render/src/surface/vk_no_timestamps.cpp), so there are no pl_timer GPU times and each result carries the label
// `gpu_time=null`; the per-pass GPU times of §10.5.8 are not reported (they would not be gated either).
//
// A software device (lavapipe) is never gated: tools/perf_compare.py applies budgets only to a registered hardware id
// (§14.9.2), and a machine without a registered id has no budget row.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "ovt/render/picture.h"
#include "ovt/render/video_surface.h"
#include "ovt_model_generated.h"
#include "ovt_perf.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using ovt::render::DisplayTarget;
using ovt::render::Picture;
using ovt::render::VideoSurface;
using ovt::render::ViewState;

constexpr const char* kGpuLabel = "gpu_time=null (no pl_timer: timestamp queries are hidden, #304)";

// Command-line configuration, filled by parse_flags() before the benchmarks run.
struct Config {
  std::string fixture;
  int frames = 100;
  int viewport_w = 0;  // 0 = size of the picture
  int viewport_h = 0;
  int fixture_w = 0;  // coded size read from the fixture; 0 = none
  int fixture_h = 0;
};

Config& config() {
  static Config c;
  return c;
}

// ---- Synthetic picture (step 1) --------------------------------------------------------------------------------

// An owning 10-bit 4:2:0 BT.2020 PQ picture, limited range, planes as 16-bit little-endian samples. The content is a
// diagonal luma ramp up to code 767 (about 1000 cd/m² in PQ) with deterministic noise in the low bits and slowly
// varying chroma, so neither the upload nor the shaders see a constant picture. Fully deterministic (fixed xorshift
// seed).
struct SyntheticPicture {
  Picture pic;
  std::vector<std::vector<uint16_t>> storage;

  SyntheticPicture(uint32_t w, uint32_t h) {
    pic.coded_width = w;
    pic.coded_height = h;
    pic.crop = ovt::fb::Rect(0, 0, static_cast<int>(w), static_cast<int>(h));
    pic.color.struct_size = sizeof(ovt_ColorDescription);
    pic.color.bit_depth = 10;
    pic.color.chroma_format = 1;
    pic.color.primaries = 9;  // BT.2020
    pic.color.transfer = 16;  // PQ
    pic.color.matrix = 9;     // BT.2020 non-constant luminance
    pic.color.full_range = 0;
    pic.color.chroma_siting = 3;  // HEVC / AV1 top-left
    pic.color.max_cll = 1000;
    pic.color.max_fall = 400;
    pic.num_planes = 3;
    pic.generation = 1;
    storage.resize(3);
    uint32_t rng = 0x9E3779B9U;
    for (uint8_t p = 0; p < 3; ++p) {
      const uint32_t pw = p == 0 ? w : (w + 1) / 2;
      const uint32_t ph = p == 0 ? h : (h + 1) / 2;
      storage.at(p).resize(static_cast<size_t>(pw) * ph);
      for (uint32_t y = 0; y < ph; ++y) {
        uint16_t* row = storage.at(p).data() + (static_cast<size_t>(y) * pw);
        for (uint32_t x = 0; x < pw; ++x) {
          rng ^= rng << 13U;
          rng ^= rng >> 17U;
          rng ^= rng << 5U;
          const uint32_t noise = rng & 0x7U;
          if (p == 0) {
            const auto ramp = static_cast<uint32_t>((static_cast<uint64_t>(x) + y) * 703U /
                                                    (static_cast<uint64_t>(pw) + ph));  // 0..703
            row[x] = static_cast<uint16_t>(64U + ramp + noise);                         // 64..774
          } else {
            row[x] = static_cast<uint16_t>(448U + (((x * 64U) / pw) ^ ((y * 64U) / ph)) + noise);
          }
        }
      }
      ovt::render::PlaneData& pd = pic.planes[p];
      pd.data = storage.at(p).data();
      pd.width = pw;
      pd.height = ph;
      pd.stride_bytes = pw * 2U;
      pd.sample_bytes = 2;
    }
  }
};

// ---- Scripted zoom/pan path (step 2) ----------------------------------------------------------------------------

// Frame i of n: zoom runs from the fit-to-viewport zoom up to 2x and back (one cosine period, so the first and last
// frame of the path are equal) while the pan follows a Lissajous curve inside the picture. The path is a pure
// function of (i, n, picture size, viewport), so every run and every machine renders the same sequence.
ViewState view_at(int i, int n, const SyntheticPicture& pic, int vw, int vh) {
  constexpr double kPi = 3.14159265358979323846;
  const double w = pic.pic.coded_width;
  const double h = pic.pic.coded_height;
  const double fit = std::min(vw / w, vh / h);
  const double t = n > 1 ? static_cast<double>(i) / n : 0.0;
  const double mix = 0.5 - (0.5 * std::cos(2.0 * kPi * t));  // 0 -> 1 -> 0
  ViewState v;
  v.zoom = fit * std::pow(2.0 / fit, mix);  // log-linear between fit and 2.0
  v.pan_x = (w / 2.0) + (0.25 * w * std::sin(2.0 * kPi * t));
  v.pan_y = (h / 2.0) + (0.25 * h * std::sin(4.0 * kPi * t));
  v.tone_map = ovt::render::ToneMap::BT2390;
  v.show_sample_values = false;
  return v;
}

// ---- Statistics (step 3) ----------------------------------------------------------------------------------------

// Nearest-rank percentile (p in 0..100) of a sample set; `v` is reordered.
double percentile(std::vector<double>& v, double p) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const auto rank = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size())));
  return v.at(std::min(v.size(), std::max<size_t>(rank, 1)) - 1);
}

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---- Surface setup ---------------------------------------------------------------------------------------------

// Creates and initialises an offscreen surface of w x h (Rgba8Srgb, SDR). nullptr plus an error text on failure.
std::unique_ptr<VideoSurface> make_surface(int w, int h, std::string* error) {
  auto surface = VideoSurface::create("bench-render");
  DisplayTarget t;
  t.width = w;
  t.height = h;
  t.offscreen_format = DisplayTarget::OffscreenFormat::Rgba8Srgb;
  if (!surface->init(t)) {
    *error = surface->last_error();
    return nullptr;
  }
  return surface;
}

void picture_size(uint32_t default_w, uint32_t default_h, uint32_t* w, uint32_t* h) {
  const Config& c = config();
  *w = c.fixture_w > 0 ? static_cast<uint32_t>(c.fixture_w) : default_w;
  *h = c.fixture_h > 0 ? static_cast<uint32_t>(c.fixture_h) : default_h;
}

// ---- bench_render_video_4k --------------------------------------------------------------------------------------

void bench_render_video_4k(benchmark::State& state) {
  uint32_t w = 0;
  uint32_t h = 0;
  picture_size(3840, 2160, &w, &h);
  const Config& c = config();
  const int vw = c.viewport_w > 0 ? c.viewport_w : static_cast<int>(w);
  const int vh = c.viewport_h > 0 ? c.viewport_h : static_cast<int>(h);
  std::string error;
  auto surface = make_surface(vw, vh, &error);
  if (!surface) {
    state.SkipWithError(("VideoSurface::init failed: " + error).c_str());
    return;
  }
  const SyntheticPicture pic(w, h);
  surface->set_frame(pic.pic);
  // Warm-up: shader compilation, LUT build and the first upload belong to bench_render_first_frame, not to this row.
  for (int i = 0; i < 5; ++i) {
    surface->render(view_at(0, c.frames, pic, vw, vh));
  }
  std::vector<double> samples;
  for (auto _ : state) {
    for (int i = 0; i < c.frames; ++i) {
      const ViewState view = view_at(i, c.frames, pic, vw, vh);
      const auto t0 = Clock::now();
      surface->render(view);
      samples.push_back(ms_since(t0));
    }
  }
  state.counters["p50_ms"] = percentile(samples, 50.0);
  state.counters["p95_ms"] = percentile(samples, 95.0);
  state.counters["p99_ms"] = percentile(samples, 99.0);
  state.counters["frames"] = static_cast<double>(samples.size());
  state.SetLabel(std::string(kGpuLabel) + "; " + std::to_string(w) + "x" + std::to_string(h) + " -> " +
                 std::to_string(vw) + "x" + std::to_string(vh));
}
OVT_BENCHMARK(bench_render_video_4k);

// ---- bench_render_first_frame -----------------------------------------------------------------------------------

// The first creation in the process is the cold sample: libovt-render has no on-disk shader cache of its own, so cold
// is what a freshly started process pays (shader compilation, LUT build, driver pipeline cache empty or cold), and warm
// is a later creation in the same process (driver caches filled). The cold sample is taken once per process; every
// repetition reports the same cold_ms and its own median warm_ms.
double& cold_sample_ms() {
  static double v = -1.0;
  return v;
}

void bench_render_first_frame(benchmark::State& state) {
  uint32_t w = 0;
  uint32_t h = 0;
  picture_size(1920, 1080, &w, &h);
  const Config& c = config();
  const int vw = c.viewport_w > 0 ? c.viewport_w : static_cast<int>(w);
  const int vh = c.viewport_h > 0 ? c.viewport_h : static_cast<int>(h);
  const SyntheticPicture pic(w, h);
  const ViewState view = view_at(0, 1, pic, vw, vh);
  std::vector<double> warm;
  for (auto _ : state) {
    std::string error;
    const auto t0 = Clock::now();
    auto surface = make_surface(vw, vh, &error);
    if (!surface) {
      state.SkipWithError(("VideoSurface::init failed: " + error).c_str());
      return;
    }
    surface->set_frame(pic.pic);
    surface->render(view);
    const double ms = ms_since(t0);
    state.PauseTiming();
    surface.reset();  // teardown is not part of the row
    state.ResumeTiming();
    if (cold_sample_ms() < 0.0) {
      cold_sample_ms() = ms;
    } else {
      warm.push_back(ms);
    }
  }
  state.counters["cold_ms"] = cold_sample_ms();
  state.counters["warm_ms"] = warm.empty() ? cold_sample_ms() : percentile(warm, 50.0);
  state.SetLabel(std::string(kGpuLabel) + "; " + std::to_string(w) + "x" + std::to_string(h));
}
OVT_BENCHMARK(bench_render_first_frame);

// ---- Flags ------------------------------------------------------------------------------------------------------

// Value of `--name V` or `--name=V` at argv[i]; advances i past a separate value. nullptr when argv[i] is another flag.
const char* flag_value(int argc, char** argv, int* i, const char* name) {
  const std::string arg = argv[*i];
  const std::string prefix = std::string(name) + "=";
  if (arg == name) {
    if (*i + 1 >= argc) {
      throw std::runtime_error(std::string(name) + " needs a value");
    }
    return argv[++*i];
  }
  if (arg.rfind(prefix, 0) == 0) {
    return argv[*i] + prefix.size();
  }
  return nullptr;
}

int parse_int(const char* text, const char* what, int min) {
  char* end = nullptr;
  const long v = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || v < min || v > 65536) {
    throw std::runtime_error(std::string(what) + ": invalid value '" + text + "'");
  }
  return static_cast<int>(v);
}

void load_fixture(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("--fixture: cannot open " + path);
  }
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  if (!ovt::fb::VerifyFrameAnalysisBuffer(verifier)) {
    throw std::runtime_error("--fixture: " + path + " is not a FrameAnalysis FlatBuffer");
  }
  const ovt::fb::FrameAnalysis* fa = ovt::fb::GetFrameAnalysis(bytes.data());
  const ovt::fb::FrameInfo* frame = fa->frame();
  const ovt::fb::Rect* size = frame != nullptr ? frame->coded_size() : nullptr;
  if (size == nullptr || size->w() <= 0 || size->h() <= 0) {
    throw std::runtime_error("--fixture: " + path + " has no coded size");
  }
  config().fixture_w = size->w();
  config().fixture_h = size->h();
}

// Removes this benchmark's flags from argv and fills config(); the rest goes to Google Benchmark.
void parse_flags(int* argc, char** argv) {
  Config& c = config();
  int out = 1;
  for (int i = 1; i < *argc; ++i) {
    if (const char* v = flag_value(*argc, argv, &i, "--fixture")) {
      c.fixture = v;
    } else if (const char* f = flag_value(*argc, argv, &i, "--frames")) {
      c.frames = parse_int(f, "--frames", 1);
    } else if (const char* vp = flag_value(*argc, argv, &i, "--viewport")) {
      const std::string text = vp;
      const size_t x = text.find('x');
      if (x == std::string::npos) {
        throw std::runtime_error("--viewport: expected WxH, got '" + text + "'");
      }
      c.viewport_w = parse_int(text.substr(0, x).c_str(), "--viewport width", 1);
      c.viewport_h = parse_int(text.substr(x + 1).c_str(), "--viewport height", 1);
    } else {
      argv[out++] = argv[i];
    }
  }
  *argc = out;
  if (!c.fixture.empty()) {
    load_fixture(c.fixture);
  }
}

int run(int argc, char** argv) {
  parse_flags(&argc, argv);
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    (void)std::fprintf(stderr, "bench_render_video: %s\n", e.what());
    return 2;
  }
}
