// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the psnr metric (chapter 9 §9.4.2, §9.4.7; WI-3.306 acceptance tests 1-3).

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

#include "metrics/host/plugin_registry.h"
#include "metrics/kernels/map_sink.h"
#include "metrics/kernels/psnr_kernels.h"
#include "metrics/kernels/psnr_metric.h"
#include "ovt_metrics_generated.h"
#include "test_support/corpus.h"

namespace {

using ovt::metrics::psnr_metric_api;

// A planar picture in the layout of ovt_MetricPlanes (sample_bytes 1 or 2).
struct Pic {
  std::array<std::vector<uint8_t>, 3> buf;
  ovt_MetricPlanes planes{};
  Pic(uint32_t w, uint32_t h, int chroma /*0 400, 1 420, 3 444*/, uint32_t bits, uint32_t pad = 0) {
    planes.struct_size = sizeof(ovt_MetricPlanes);
    planes.planes = chroma == 0 ? 1 : 3;
    planes.bit_depth = static_cast<uint8_t>(bits);
    planes.sample_bytes = bits > 8 ? 2 : 1;
    planes.sample_format = OVT_METRIC_SAMPLE_UINT;
    planes.domain = OVT_METRIC_DOMAIN_CODE_VALUE;
    for (size_t p = 0; p < planes.planes; ++p) {
      const uint32_t pw = (p == 0 || chroma == 3) ? w : (w + 1) / 2;
      const uint32_t ph = (p == 0 || chroma == 3) ? h : (h + 1) / 2;
      const uint32_t stride = (pw * planes.sample_bytes) + pad;
      buf[p].assign(static_cast<size_t>(stride) * ph, 0);
      planes.data[p] = buf[p].data();
      planes.stride_bytes[p] = static_cast<int32_t>(stride);
      planes.width[p] = pw;
      planes.height[p] = ph;
    }
  }
  Pic(const Pic&) = delete;
  Pic& operator=(const Pic&) = delete;
  void set(size_t p, uint32_t x, uint32_t y, uint32_t v) {
    const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(planes.stride_bytes[p])) +
                       (static_cast<size_t>(x) * planes.sample_bytes);
    buf[p][off] = static_cast<uint8_t>(v & 0xFFU);
    if (planes.sample_bytes == 2) {
      buf[p][off + 1] = static_cast<uint8_t>(v >> 8U);
    }
  }
  static uint32_t draw(std::mt19937& rng, uint32_t bound) { return static_cast<uint32_t>(rng() % bound); }
  void randomize(std::mt19937& rng) {
    const uint32_t maxv = (1U << planes.bit_depth) - 1U;
    for (size_t p = 0; p < planes.planes; ++p) {
      for (uint32_t y = 0; y < planes.height[p]; ++y) {
        for (uint32_t x = 0; x < planes.width[p]; ++x) {
          set(p, x, y, draw(rng, maxv + 1U));
        }
      }
    }
  }
};

struct Scored {
  std::vector<double> values;
  std::string map_blob_id;
  uint16_t map_block_size = 0;
};

Scored decode(const ovt_Buffer& b) {
  const auto* fs = flatbuffers::GetRoot<ovt::fb::FrameScore>(b.data);
  Scored s;
  if (fs->values() != nullptr) {
    s.values.assign(fs->values()->begin(), fs->values()->end());
  }
  if (fs->map_blob_id() != nullptr) {
    s.map_blob_id = fs->map_blob_id()->str();
  }
  s.map_block_size = fs->map_block_size();
  return s;
}

struct Instance {
  const ovt_MetricApi* api = psnr_metric_api();
  ovt_Metric* m = nullptr;
  explicit Instance(const char* options = "{}") { REQUIRE(api->create(options, &m) == OVT_OK); }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  ~Instance() { api->destroy(m); }
  [[nodiscard]] Scored score(const Pic& r, const Pic& d, int32_t idx = 0) const {
    ovt_Buffer out{};
    REQUIRE(api->score(m, &r.planes, &d.planes, ovt_FrameId{idx, idx}, &out) == OVT_OK);
    REQUIRE(out.data != nullptr);
    return decode(out);
  }
  [[nodiscard]] Scored finish() const {
    ovt_Buffer out{};
    REQUIRE(api->finish(m, &out) == OVT_OK);
    REQUIRE(out.data != nullptr);
    return decode(out);
  }
};

class CaptureSink final : public ovt::metrics::MapSink {
 public:
  void put(const std::string& id, std::vector<uint8_t> bytes) override {
    const std::lock_guard lock(mu);
    maps[id] = std::move(bytes);
  }
  std::mutex mu;
  std::map<std::string, std::vector<uint8_t>> maps;
};

float map_value(const std::vector<uint8_t>& blob, size_t i) {
  float f = 0;
  std::memcpy(&f, blob.data() + ovt::metrics::kOvtMapHeaderSize + (i * 4), 4);
  return f;
}

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

TEST_CASE("psnr descriptor and registration", "[metrics][psnr][fast]") {
  std::string err;
  const std::string id = "psnr";
  const auto bytes = ovt::metrics::validate_metric_api(psnr_metric_api(), &id, err);
  REQUIRE(bytes.has_value());
  const auto* d = flatbuffers::GetRoot<ovt::fb::MetricDescriptor>(bytes->data());
  CHECK(d->id()->str() == "psnr");
  CHECK(d->per_plane());
  CHECK(d->higher_is_better());
  CHECK(d->domain() == ovt::fb::WorkingDomain::CodeValue);
  CHECK(d->outputs()->Get(0)->str() == "y");
  ovt::metrics::PluginRegistry reg(ovt::metrics::PluginRegistry::Options{
      .ovt_metric_path = "", .ovt_plugin_path = "", .defaults = std::vector<std::string>{}});
  ovt::metrics::register_builtin_kernels(reg);
  CHECK(reg.find("psnr") != nullptr);
}

TEST_CASE("psnr rejects unknown options and bad inputs", "[metrics][psnr][fast]") {
  const auto* api = psnr_metric_api();
  ovt_Metric* m = nullptr;
  CHECK(api->create(R"({"nope":1})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"map_block":12})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"planes":["q"]})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"psnr_max_db":-1})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create("not json", &m) == OVT_E_INVALID_ARG);
  Instance inst;
  Pic a(16, 16, 1, 8);
  Pic b(32, 16, 1, 8);
  ovt_Buffer out{};
  CHECK(api->score(inst.m, &a.planes, &b.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  CHECK(api->score(inst.m, nullptr, &b.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
}

TEST_CASE("psnr formulae: planes, combinations, MSE, psnr_max and APSNR", "[metrics][psnr][fast]") {
  // 8-bit 4:2:0 16x16: Y differs by 2 everywhere (MSE 4), U by 1 (MSE 1), V identical (MSE 0 -> 60 dB).
  Pic r(16, 16, 1, 8);
  Pic d(16, 16, 1, 8);
  for (size_t p = 0; p < 3; ++p) {
    for (uint32_t y = 0; y < r.planes.height[p]; ++y) {
      for (uint32_t x = 0; x < r.planes.width[p]; ++x) {
        r.set(p, x, y, 100);
        d.set(p, x, y, p == 0 ? 102 : (p == 1 ? 101 : 100));
      }
    }
  }
  Instance inst;
  const Scored s = inst.score(r, d, 7);
  REQUIRE(s.values.size() == 8);
  const double py = 10 * std::log10(255.0 * 255.0 / 4.0);
  const double pu = 10 * std::log10(255.0 * 255.0 / 1.0);
  CHECK(s.values[0] == Catch::Approx(py).margin(1e-12));
  CHECK(s.values[1] == Catch::Approx(pu).margin(1e-12));
  CHECK(s.values[2] == 60.0);
  CHECK(s.values[3] == Catch::Approx((6 * py + pu + 60.0) / 8).margin(1e-12));
  // ffmpeg_avg is MSE-weighted over sample counts (256 + 64 + 64), not the mean of the dB values.
  const double mse_avg = (4.0 * 256 + 1.0 * 64) / 384.0;
  CHECK(s.values[4] == Catch::Approx(10 * std::log10(255.0 * 255.0 / mse_avg)).margin(1e-12));
  CHECK(s.values[5] == 4.0);
  CHECK(s.values[6] == 1.0);
  CHECK(s.values[7] == 0.0);
  // APSNR is computed from the mean MSE over frames: add a second frame with Y MSE 16.
  Pic d2(16, 16, 1, 8);
  for (uint32_t y = 0; y < 16; ++y) {
    for (uint32_t x = 0; x < 16; ++x) {
      d2.set(0, x, y, 104);
    }
  }
  for (size_t p = 1; p < 3; ++p) {
    for (uint32_t y = 0; y < 8; ++y) {
      for (uint32_t x = 0; x < 8; ++x) {
        d2.set(p, x, y, 100);
      }
    }
  }
  (void)inst.score(r, d2, 8);
  const Scored f = inst.finish();
  REQUIRE(f.values.size() == 4);
  CHECK(f.values[0] == Catch::Approx(10 * std::log10(255.0 * 255.0 / 10.0)).margin(1e-12));  // mean(4, 16)
  CHECK(f.values[2] == 60.0);
  CHECK(f.values[3] == Catch::Approx((6 * f.values[0] + f.values[1] + f.values[2]) / 8).margin(1e-12));
}

TEST_CASE("psnr clips at psnr_max and honours the option", "[metrics][psnr][fast]") {
  Pic r(576, 324, 0, 8);
  Pic d(576, 324, 0, 8);
  d.set(0, 0, 0, 1);  // SSE 1: the formula gives ~100 dB, clipped to 60
  Instance clipped;
  const Scored a = clipped.score(r, d);
  CHECK(a.values[0] == 60.0);
  CHECK(a.values[3] == 60.0);  // 4:0:0: combinations equal Y
  CHECK(a.values[4] == 60.0);
  CHECK(std::isnan(a.values[1]));
  Instance wide(R"({"psnr_max_db":120})");
  CHECK(wide.score(r, d).values[0] == Catch::Approx(10 * std::log10(255.0 * 255.0 * 576 * 324)).margin(1e-9));
  Instance only_y(R"({"planes":["y"]})");
  Pic r3(16, 16, 1, 8);
  Pic d3(16, 16, 1, 8);
  d3.set(0, 0, 0, 3);
  const Scored y = only_y.score(r3, d3);
  CHECK(std::isfinite(y.values[0]));
  CHECK(std::isnan(y.values[1]));
  CHECK(std::isnan(y.values[3]));
}

TEST_CASE("psnr identical 10-bit inputs give 72 dB and an all-72 map", "[metrics][psnr][fast]") {
  CaptureSink sink;
  ovt::metrics::set_map_sink(&sink);
  Pic r(70, 40, 1, 10);
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      5);
  r.randomize(rng);
  Instance inst(R"({"map_block":16})");
  const Scored s = inst.score(r, r, 12);
  ovt::metrics::set_map_sink(nullptr);
  REQUIRE(s.values.size() == 8);
  for (size_t i : {0U, 1U, 2U, 3U, 4U}) {
    CHECK(s.values[i] == 72.0);
  }
  CHECK(s.values[5] == 0.0);
  CHECK(s.map_blob_id == "psnr/y/000012/16");
  CHECK(s.map_block_size == 16);
  REQUIRE(sink.maps.size() == 3);
  const auto& blob = sink.maps.at("psnr/y/000012/16");
  REQUIRE(blob.size() == 32 + (5U * 3U * 4U));  // ceil(70/16) x ceil(40/16)
  CHECK(std::memcmp(blob.data(), "OVTMAP01", 8) == 0);
  for (size_t i = 0; i < 15; ++i) {
    CHECK(map_value(blob, i) == 72.0F);
  }
  CHECK(sink.maps.at("psnr/u/000012/16").size() == blob.size());
}

TEST_CASE("psnr block map values equal the PSNR of each block", "[metrics][psnr][fast]") {
  CaptureSink sink;
  ovt::metrics::set_map_sink(&sink);
  Pic r(20, 20, 3, 8);
  Pic d(20, 20, 3, 8);
  d.set(0, 9, 3, 8);  // inside block (1,0) of the 8x8 grid: SSE 64 over 64 samples -> MSE 1
  Instance inst(R"({"map_block":8})");
  (void)inst.score(r, d, 0);
  ovt::metrics::set_map_sink(nullptr);
  const auto& blob = sink.maps.at("psnr/y/000000/8");
  REQUIRE(blob.size() == 32 + (3U * 3U * 4U));
  CHECK(map_value(blob, 0) == 60.0F);
  CHECK(map_value(blob, 1) == Catch::Approx(10 * std::log10(255.0 * 255.0)).margin(1e-4));
  CHECK(map_value(blob, 8) == 60.0F);  // the 4x4 edge block holds only existing samples
}

TEST_CASE("psnr scalar and Highway SSE are bit-identical on 100 random frames", "[metrics][psnr][fast]") {
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      12345);
  const std::array<uint32_t, 4> depths = {8, 10, 12, 16};
  for (int i = 0; i < 100; ++i) {
    const uint32_t bits = depths[static_cast<size_t>(i) % 4];
    const uint32_t w = 1 + Pic::draw(rng, 300);
    const uint32_t h = 1 + Pic::draw(rng, 70);
    Pic r(w, h, 0, bits, 3 * Pic::draw(rng, 5));  // padded strides, odd sizes
    Pic d(w, h, 0, bits, 3 * Pic::draw(rng, 5));
    r.randomize(rng);
    d.randomize(rng);
    if (i % 3 == 0) {  // a near-identical pair keeps the sums small
      d.buf[0] = r.buf[0];
      d.planes.stride_bytes[0] = r.planes.stride_bytes[0];
      d.planes.data[0] = d.buf[0].data();
      d.set(0, 0, 0, 1);
    }
    const auto a = ovt::metrics::psnr_sse_scalar(r.planes.data[0], r.planes.stride_bytes[0], d.planes.data[0],
                                                 d.planes.stride_bytes[0], w, h, r.planes.sample_bytes, bits);
    const auto b = ovt::metrics::psnr_sse_hwy(r.planes.data[0], r.planes.stride_bytes[0], d.planes.data[0],
                                              d.planes.stride_bytes[0], w, h, r.planes.sample_bytes, bits);
    REQUIRE(a == b);
  }
  // Worst case at 16 bits: every difference is 65535 (uint32 lane accumulators must not wrap).
  Pic r(777, 5, 0, 16);
  Pic d(777, 5, 0, 16);
  for (uint32_t y = 0; y < 5; ++y) {
    for (uint32_t x = 0; x < 777; ++x) {
      d.set(0, x, y, 65535);
    }
  }
  const auto a = ovt::metrics::psnr_sse_scalar(r.planes.data[0], r.planes.stride_bytes[0], d.planes.data[0],
                                               d.planes.stride_bytes[0], 777, 5, 2, 16);
  CHECK(a == 777ULL * 5 * 65535ULL * 65535ULL);
  CHECK(a == ovt::metrics::psnr_sse_hwy(r.planes.data[0], r.planes.stride_bytes[0], d.planes.data[0],
                                        d.planes.stride_bytes[0], 777, 5, 2, 16));
}

TEST_CASE("psnr matches libvmaf and FFmpeg on the Netflix pair", "[metrics][psnr][golden][needs-corpus]") {
  const auto ref_path = ovt::test::corpus_or_skip("netflix_src01_hrc00_576x324");
  const auto dist_path = ovt::test::corpus_or_skip("netflix_src01_hrc01_576x324");
  std::ifstream gf(std::filesystem::path(OVT_METRICS_GOLDEN_DIR) / "psnr_netflix_src01_576x324.json");
  REQUIRE(gf);
  const auto golden = nlohmann::json::parse(gf);
  Y4m ry(ref_path);
  Y4m dy(dist_path);
  Pic r(ry.w_, ry.h_, 1, 8);
  Pic d(dy.w_, dy.h_, 1, 8);
  Instance inst;
  size_t n = 0;
  while (ry.next(r) && dy.next(d)) {
    REQUIRE(n < golden.at("frames").size());
    const auto& g = golden.at("frames")[n];
    const Scored s = inst.score(r, d, static_cast<int32_t>(n));
    const auto& lv = g.at("libvmaf");
    CHECK(std::fabs(s.values[0] - lv.at("psnr_y").get<double>()) <= 1e-6);
    CHECK(std::fabs(s.values[1] - lv.at("psnr_cb").get<double>()) <= 1e-6);
    CHECK(std::fabs(s.values[2] - lv.at("psnr_cr").get<double>()) <= 1e-6);
    // FFmpeg prints two decimals: compare within half a unit of the last printed digit.
    const auto& ff = g.at("ffmpeg");
    CHECK(std::fabs(s.values[0] - ff.at("psnr_y").get<double>()) <= 0.0051);
    CHECK(std::fabs(s.values[4] - ff.at("psnr_avg").get<double>()) <= 0.0051);
    ++n;
  }
  CHECK(n == 48);
  CHECK(n == golden.at("frames").size());
}

// Hidden (run with "[bench]"): single-thread throughput of the psnr metric on a 1080p 8-bit 4:2:0 pair, the quantity of
// the chapter 14 §14.9.3 budget bench_metrics_psnr_1080p (fps_1_thread >= 600 on m1-air-8gb). WI-0.560's benchmark
// binaries wrap the same call; this prints the number for the manual record docs/acceptance/wi/WI-3.306.md.
TEST_CASE("psnr 1080p single-thread throughput", "[.][bench][metrics][psnr]") {
  Pic r(1920, 1080, 1, 8);
  Pic d(1920, 1080, 1, 8);
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      1);
  r.randomize(rng);
  d.randomize(rng);
  Instance inst;
  constexpr int kFrames = 200;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kFrames; ++i) {
    (void)inst.score(r, d, i);
  }
  const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
  std::printf("psnr 1080p: %.1f fps (1 thread)\n", kFrames / dt.count());
  SUCCEED();
}
