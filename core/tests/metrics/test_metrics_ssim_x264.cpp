// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the ssim_x264 metric (chapter 9 §9.4.4, §9.4.7; WI-3.308 acceptance test 1 and the ssim_db_y clause):
// registration, options, an independent per-window reference, the x264 kernel oracle on synthetic 8-bit and 10-bit
// pairs and on the Netflix pair, ssim_db_y, odd strides and 4:0:0, and the maps.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

#include "metrics/host/plugin_registry.h"
#include "metrics/kernels/map_sink.h"
#include "metrics/kernels/psnr_metric.h"
#include "metrics/kernels/ssim_x264.h"
#include "metrics_test_pic.hpp"
#include "ovt_metrics_generated.h"
#include "test_support/corpus.h"

namespace {

using ovt::metrics::ssim_x264_metric_api;
using ovt::test::CaptureSink;
using ovt::test::decode_score;
using ovt::test::Pic;
using ovt::test::Scored;

struct Instance {
  const ovt_MetricApi* api = ssim_x264_metric_api();
  ovt_Metric* m = nullptr;
  explicit Instance(const char* options = "{}") { REQUIRE(api->create(options, &m) == OVT_OK); }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  ~Instance() { api->destroy(m); }
  [[nodiscard]] Scored score(const Pic& r, const Pic& d, int32_t idx = 0) const {
    ovt_Buffer out{};
    REQUIRE(api->score(m, &r.planes, &d.planes, ovt_FrameId{idx, idx}, &out) == OVT_OK);
    REQUIRE(out.data != nullptr);
    return decode_score(out);
  }
};

// §9.4.4 transcribed with doubles on the raw samples, one window at a time: no block sums, no rolling rows.
double reference_plane(const Pic& r, const Pic& d, size_t p) {
  const uint32_t bits = r.planes.bit_depth;
  const auto peak = static_cast<double>((1U << bits) - 1U);
  const bool integer_branch = bits <= 9;
  const double c1 = integer_branch ? std::floor((0.01 * 0.01 * peak * peak * 64.0) + 0.5)
                                   : static_cast<double>(static_cast<float>(0.01 * 0.01 * peak * peak * 64.0));
  const double c2 = integer_branch ? std::floor((0.03 * 0.03 * peak * peak * 64.0 * 63.0) + 0.5)
                                   : static_cast<double>(static_cast<float>(0.03 * 0.03 * peak * peak * 64.0 * 63.0));
  const uint32_t bx = (r.planes.width[p] - 2) / 4;
  const uint32_t by = (r.planes.height[p] - 2) / 4;
  double total = 0;
  for (uint32_t wy = 0; wy + 1 < by; ++wy) {
    for (uint32_t wx = 0; wx + 1 < bx; ++wx) {
      double s1 = 0;
      double s2 = 0;
      double ss = 0;
      double s12 = 0;
      for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
          const double a = r.get(p, 2 + (4 * wx) + x, 2 + (4 * wy) + y);
          const double b = d.get(p, 2 + (4 * wx) + x, 2 + (4 * wy) + y);
          s1 += a;
          s2 += b;
          ss += (a * a) + (b * b);
          s12 += a * b;
        }
      }
      const double vars = (ss * 64) - (s1 * s1) - (s2 * s2);
      const double covar = (s12 * 64) - (s1 * s2);
      total += (((2 * s1 * s2) + c1) * ((2 * covar) + c2)) / (((s1 * s1) + (s2 * s2) + c1) * (vars + c2));
    }
  }
  return total / (static_cast<double>(bx - 1) * static_cast<double>(by - 1));
}

void fill_noise(Pic& pic, std::mt19937& rng, bool distort_of_ref = false, const Pic* ref = nullptr) {
  const uint32_t top = (1U << pic.planes.bit_depth) - 1U;
  for (size_t p = 0; p < pic.planes.planes; ++p) {
    for (uint32_t y = 0; y < pic.planes.height[p]; ++y) {
      for (uint32_t x = 0; x < pic.planes.width[p]; ++x) {
        uint32_t v = static_cast<uint32_t>(rng()) % (top + 1U);
        if (distort_of_ref && ref != nullptr) {
          const int32_t jitter = static_cast<int32_t>(rng() % 33U) - 16;
          v = static_cast<uint32_t>(
              std::clamp<int32_t>(static_cast<int32_t>(ref->get(p, x, y)) + jitter, 0, static_cast<int32_t>(top)));
        }
        pic.set(p, x, y, v);
      }
    }
  }
}

}  // namespace

TEST_CASE("ssim_x264 is registered with a valid descriptor", "[metrics][ssim_x264][fast]") {
  std::string err;
  const std::string id = "ssim_x264";
  const auto bytes = ovt::metrics::validate_metric_api(ssim_x264_metric_api(), &id, err);
  REQUIRE(bytes.has_value());
  const auto* d = flatbuffers::GetRoot<ovt::fb::MetricDescriptor>(bytes->data());
  CHECK(d->id()->str() == "ssim_x264");
  CHECK(d->per_plane());
  CHECK(d->higher_is_better());
  CHECK(d->domain() == ovt::fb::WorkingDomain::CodeValue);
  REQUIRE(d->outputs()->size() == 5);
  CHECK(d->outputs()->Get(3)->str() == "ssim_db_y");
  ovt::metrics::PluginRegistry reg(ovt::metrics::PluginRegistry::Options{
      .ovt_metric_path = "", .ovt_plugin_path = "", .defaults = std::vector<std::string>{}});
  ovt::metrics::register_builtin_kernels(reg);
  CHECK(reg.find("ssim_x264") != nullptr);
  CHECK(reg.find("identity") != nullptr);
  CHECK(reg.find("ssim") != nullptr);
  CHECK(reg.find("psnr") != nullptr);
}

TEST_CASE("ssim_x264 rejects unknown options and bad inputs", "[metrics][ssim_x264][fast]") {
  const auto* api = ssim_x264_metric_api();
  ovt_Metric* m = nullptr;
  CHECK(api->create(R"({"nope":1})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"border":"valid"})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"map_block":12})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"planes":["q"]})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"planes":[]})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create("not json", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"planes":["y","u","v"],"map_block":16})", &m) == OVT_OK);
  api->destroy(m);
  Instance inst;
  Pic a(32, 32, 1, 8);
  Pic b(48, 32, 1, 8);
  ovt_Buffer out{};
  CHECK(api->score(inst.m, &a.planes, &b.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  CHECK(api->score(inst.m, nullptr, &b.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  // A plane with fewer than 2 x 2 blocks after the 2-sample offset has no window (SPEC-ASSUMPTION #419).
  Pic tiny(9, 40, 0, 8);
  CHECK(api->score(inst.m, &tiny.planes, &tiny.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  Pic small(10, 10, 0, 8);  // (10 - 2) / 4 = 2 blocks each way: exactly one window
  CHECK(api->score(inst.m, &small.planes, &small.planes, ovt_FrameId{0, 0}, &out) == OVT_OK);
  // 4:0:0 input with ["u"] selects no plane.
  Instance only_u(R"({"planes":["u"]})");
  Pic gray(32, 32, 0, 8);
  CHECK(api->score(only_u.m, &gray.planes, &gray.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
}

TEST_CASE("ssim_x264 equals the per-window reference at 8, 9, 10 and 12 bits", "[metrics][ssim_x264][fast]") {
  std::mt19937 rng(7);  // NOLINT(cert-msc32-c,cert-msc51-cpp): a fixed seed makes the case reproducible
  struct Shape {
    uint32_t w, h, bits;
    uint32_t pad;
  };
  for (const Shape s :
       {Shape{26, 26, 8, 0}, Shape{67, 45, 8, 5}, Shape{50, 34, 9, 0}, Shape{66, 42, 10, 6}, Shape{35, 27, 12, 2}}) {
    Pic r(s.w, s.h, 1, s.bits, s.pad);
    Pic d(s.w, s.h, 1, s.bits, s.pad);
    fill_noise(r, rng);
    fill_noise(d, rng, true, &r);
    Instance inst(R"({"planes":["y","u","v"]})");
    const Scored got = inst.score(r, d);
    REQUIRE(got.values.size() == 4);
    for (size_t p = 0; p < 3; ++p) {
      INFO(s.w << "x" << s.h << " " << s.bits << "-bit plane " << p);
      const double want = reference_plane(r, d, p);
      // The 10/12-bit branch takes float products in x264's order; the reference is exact double: documented <= 1e-6
      // on a plane mean (docs/metrics_differences.md), looser per window.
      CHECK(std::fabs(got.values[p] - want) <= (s.bits <= 9 ? 1e-12 : 1e-4));
    }
    CHECK(std::fabs(got.values[3] - (-10.0 * std::log10(1.0 - got.values[0]))) <= 1e-9);
  }
}

TEST_CASE("ssim_x264 on an identical pair is 1 and ssim_db_y is exactly 100", "[metrics][ssim_x264][fast]") {
  for (const uint32_t bits : {8U, 10U}) {
    Pic r(330, 194, 1, bits);
    ovt::test::fill_synth(r, 0, false);
    Instance inst(R"({"planes":["y","u","v"]})");
    const Scored s = inst.score(r, r);
    INFO(bits << "-bit");
    CHECK(std::fabs(s.values[0] - 1.0) <= 1e-6);
    CHECK(std::fabs(s.values[1] - 1.0) <= 1e-6);
    CHECK(std::fabs(s.values[2] - 1.0) <= 1e-6);
    CHECK(s.values[3] == 100.0);
  }
}

TEST_CASE("ssim_x264 planes option and NaN for planes not computed", "[metrics][ssim_x264][fast]") {
  Pic r(66, 66, 1, 8);
  Pic d(66, 66, 1, 8);
  ovt::test::fill_synth(r, 0, false);
  ovt::test::fill_synth(d, 0, true);
  const Scored y_only = Instance().score(r, d);
  CHECK(!std::isnan(y_only.values[0]));
  CHECK(std::isnan(y_only.values[1]));
  CHECK(std::isnan(y_only.values[2]));
  CHECK(!std::isnan(y_only.values[3]));
  const Scored u_only = Instance(R"({"planes":["u"]})").score(r, d);
  CHECK(std::isnan(u_only.values[0]));
  CHECK(!std::isnan(u_only.values[1]));
  CHECK(std::isnan(u_only.values[3]));  // ssim_db_y needs the luma plane
  Pic g(66, 66, 0, 8);
  Pic g2(66, 66, 0, 8);
  ovt::test::fill_synth(g, 0, false);
  ovt::test::fill_synth(g2, 0, true);
  CHECK(Instance().score(g, g2).values[0] == y_only.values[0]);  // the luma plane scores alike with or without chroma
}

TEST_CASE("ssim_x264 equals the x264 kernel oracle on the synthetic pairs", "[metrics][ssim_x264][golden][fast]") {
  std::ifstream gf(std::filesystem::path(OVT_METRICS_GOLDEN_DIR) / "ssim_x264_synth.json");
  REQUIRE(gf);
  const auto golden = nlohmann::json::parse(gf);
  for (const auto& c : golden.at("cases")) {
    const auto w = c.at("width").get<uint32_t>();
    const auto h = c.at("height").get<uint32_t>();
    const auto bits = c.at("bit_depth").get<uint32_t>();
    Pic r(w, h, 1, bits);
    Pic d(w, h, 1, bits);
    Instance inst(R"({"planes":["y","u","v"]})");
    for (uint32_t f = 0; f < c.at("y").size(); ++f) {
      ovt::test::fill_synth(r, f, false);
      ovt::test::fill_synth(d, f, true);
      const Scored s = inst.score(r, d, static_cast<int32_t>(f));
      INFO(c.at("name").get<std::string>() << " frame " << f);
      CHECK(std::fabs(s.values[0] - c.at("y")[f].get<double>()) <= 1e-6);
      CHECK(std::fabs(s.values[1] - c.at("u")[f].get<double>()) <= 1e-6);
      CHECK(std::fabs(s.values[2] - c.at("v")[f].get<double>()) <= 1e-6);
    }
  }
}

namespace {
// Reads the frames of a y4m file (420, 8-bit) into Pic objects one at a time.
class Y4m {
 public:
  explicit Y4m(const std::filesystem::path& p) : in_(p, std::ios::binary) {
    std::string header;
    std::getline(in_, header);
    REQUIRE(header.rfind("YUV4MPEG2", 0) == 0);
    std::sscanf(header.c_str(), "YUV4MPEG2 W%u H%u", &w_, &h_);  // NOLINT(cert-err34-c)
  }
  bool next(Pic& pic) {
    std::string line;
    if (!std::getline(in_, line)) {
      return false;
    }
    for (auto& plane : pic.buf) {
      in_.read(reinterpret_cast<char*>(plane.data()), static_cast<std::streamsize>(plane.size()));  // NOLINT
    }
    return static_cast<bool>(in_);
  }
  uint32_t w_ = 0;
  uint32_t h_ = 0;

 private:
  std::ifstream in_;
};
}  // namespace

TEST_CASE("ssim_x264 equals the x264 kernel oracle on the Netflix pair", "[metrics][ssim_x264][golden][needs-corpus]") {
  const auto ref_path = ovt::test::corpus_or_skip("netflix_src01_hrc00_576x324");
  const auto dist_path = ovt::test::corpus_or_skip("netflix_src01_hrc01_576x324");
  std::ifstream gf(std::filesystem::path(OVT_METRICS_GOLDEN_DIR) / "ssim_x264_netflix_src01_576x324.json");
  REQUIRE(gf);
  const auto golden = nlohmann::json::parse(gf);
  Y4m ry(ref_path);
  Y4m dy(dist_path);
  Pic r(ry.w_, ry.h_, 1, 8);
  Pic d(dy.w_, dy.h_, 1, 8);
  Instance inst(R"({"planes":["y","u","v"]})");
  size_t n = 0;
  while (ry.next(r) && dy.next(d)) {
    REQUIRE(n < golden.at("y").size());
    const Scored s = inst.score(r, d, static_cast<int32_t>(n));
    INFO("frame " << n);
    // The golden prints six decimals (tests/oracles/normalize/canon.py), so 1e-6 is the tightest comparison it allows.
    CHECK(std::fabs(s.values[0] - golden.at("y")[n].get<double>()) <= 1e-6);
    CHECK(std::fabs(s.values[1] - golden.at("u")[n].get<double>()) <= 1e-6);
    CHECK(std::fabs(s.values[2] - golden.at("v")[n].get<double>()) <= 1e-6);
    ++n;
  }
  CHECK(n == 48);
  CHECK(n == golden.at("y").size());
}

TEST_CASE("ssim_x264 maps hold the mean of the windows centred in each block", "[metrics][ssim_x264][fast]") {
  CaptureSink sink;
  ovt::metrics::set_map_sink(&sink);
  Pic r(66, 34, 1, 8);
  Pic d(66, 34, 1, 8);
  ovt::test::fill_synth(r, 0, false);
  ovt::test::fill_synth(d, 0, true);
  Instance inst(R"({"planes":["y","v"],"map_block":16})");
  const Scored s = inst.score(r, d, 12);
  ovt::metrics::set_map_sink(nullptr);
  CHECK(s.map_blob_id == "ssim_x264/y/000012/16");
  CHECK(s.map_block_size == 16);
  REQUIRE(sink.maps.size() == 2);
  const auto& y = sink.maps.at("ssim_x264/y/000012/16");
  REQUIRE(sink.maps.count("ssim_x264/v/000012/16") == 1);
  REQUIRE(y.size() == ovt::metrics::kOvtMapHeaderSize + (size_t{5} * 3U * 4U));  // ceil(66/16) x ceil(34/16) float32
  CHECK(std::memcmp(y.data(), "OVTMAP01", 8) == 0);
  // Block (0, 0) holds the windows whose centre (2 + 4 wx + 4, 2 + 4 wy + 4) is below 16: wx, wy in 0..2. Each such
  // window, cut out with its 2-sample margin as a 10 x 10 plane, is exactly one window of the kernel.
  double sum = 0;
  uint32_t n = 0;
  for (uint32_t wy = 0; wy < 3; ++wy) {
    for (uint32_t wx = 0; wx < 3; ++wx) {
      Pic rp(10, 10, 0, 8);
      Pic dp(10, 10, 0, 8);
      for (uint32_t yy = 0; yy < 8; ++yy) {
        for (uint32_t xx = 0; xx < 8; ++xx) {
          rp.set(0, xx + 2, yy + 2, r.get(0, 2 + (4 * wx) + xx, 2 + (4 * wy) + yy));
          dp.set(0, xx + 2, yy + 2, d.get(0, 2 + (4 * wx) + xx, 2 + (4 * wy) + yy));
        }
      }
      sum += Instance().score(rp, dp).values[0];
      ++n;
    }
  }
  float first = 0;
  std::memcpy(&first, y.data() + ovt::metrics::kOvtMapHeaderSize, 4);
  CHECK(std::fabs(static_cast<double>(first) - (sum / n)) <= 1e-6);
}

// WI-3.308 acceptance test 2, run by hand (docs/acceptance/wi/WI-3.308a.md): the mean luma ssim_x264 of a source and
// its x264 reconstruction equals the `SSIM Mean Y` of x264's own log within 1e-4 (the log prints 7 digits).
//   OVT_AT2_REF=src.y4m OVT_AT2_DIST=recon.y4m OVT_AT2_LOG_MEAN_Y=0.9876543 ovt_tests_metrics "[manual]"
TEST_CASE("ssim_x264 mean Y equals the SSIM Mean Y of the x264 log", "[.][manual][metrics][ssim_x264]") {
  const char* const ref_env = std::getenv("OVT_AT2_REF");         // NOLINT(concurrency-mt-unsafe)
  const char* const dist_env = std::getenv("OVT_AT2_DIST");       // NOLINT(concurrency-mt-unsafe)
  const char* const log_env = std::getenv("OVT_AT2_LOG_MEAN_Y");  // NOLINT(concurrency-mt-unsafe)
  if (ref_env == nullptr || dist_env == nullptr || log_env == nullptr) {
    SKIP("set OVT_AT2_REF, OVT_AT2_DIST and OVT_AT2_LOG_MEAN_Y");
  }
  Y4m ry{std::filesystem::path(ref_env)};
  Y4m dy{std::filesystem::path(dist_env)};
  Pic r(ry.w_, ry.h_, 1, 8);
  Pic d(dy.w_, dy.h_, 1, 8);
  Instance inst;
  double sum = 0;
  size_t n = 0;
  while (ry.next(r) && dy.next(d)) {
    sum += inst.score(r, d, static_cast<int32_t>(n)).values[0];
    ++n;
  }
  REQUIRE(n > 0);
  const double mean = sum / static_cast<double>(n);
  std::printf("frames %zu, ssim_x264/y mean %.7f, x264 log %s\n", n, mean, log_env);
  CHECK(std::fabs(mean - std::strtod(log_env, nullptr)) <= 1e-4);
}

// Hidden (run with "[bench]"): single-thread throughput of ssim_x264 on a 1080p 8-bit 4:2:0 pair (luma, and all
// planes).
TEST_CASE("ssim_x264 1080p single-thread throughput", "[.][bench][metrics][ssim_x264]") {
  Pic r(1920, 1080, 1, 8);
  Pic d(1920, 1080, 1, 8);
  ovt::test::fill_synth(r, 0, false);
  ovt::test::fill_synth(d, 0, true);
  for (const char* options : {R"({"planes":["y"]})", R"({"planes":["y","u","v"]})"}) {
    Instance inst(options);
    constexpr int kFrames = 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
      (void)inst.score(r, d, i);
    }
    const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
    std::printf("ssim_x264 1080p %s: %.1f fps (1 thread)\n", options, kFrames / dt.count());
  }
  SUCCEED();
}
