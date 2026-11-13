// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the ssim metric (chapter 9 §9.4.3, §9.4.7; WI-3.307 acceptance tests 1 and 2, plus the scalar/Highway
// identity, the libvmaf down-scaling, the maps and the "valid" border against a double-precision reference).

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
#include "metrics/kernels/psnr_metric.h"
#include "metrics/kernels/ssim_kernels.h"
#include "metrics/kernels/ssim_metric.h"
#include "ovt_metrics_generated.h"
#include "test_support/corpus.h"

namespace {

using ovt::metrics::ssim_metric_api;

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
  [[nodiscard]] uint32_t get(size_t p, uint32_t x, uint32_t y) const {
    const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(planes.stride_bytes[p])) +
                       (static_cast<size_t>(x) * planes.sample_bytes);
    return planes.sample_bytes == 2
               ? (static_cast<uint32_t>(buf[p][off]) | (static_cast<uint32_t>(buf[p][off + 1]) << 8U))
               : buf[p][off];
  }
  // Textured content: a smooth ramp plus noise, so the windows have non-zero variance.
  void fill_texture(std::mt19937& rng, uint32_t seed_shift) {
    const uint32_t scale = 1U << (planes.bit_depth - 8U);
    for (size_t p = 0; p < planes.planes; ++p) {
      for (uint32_t y = 0; y < planes.height[p]; ++y) {
        for (uint32_t x = 0; x < planes.width[p]; ++x) {
          const uint32_t v = 40U + ((3U * x + 2U * y + seed_shift) % 100U) + (static_cast<uint32_t>(rng()) % 40U);
          set(p, x, y, v * scale);
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
  const ovt_MetricApi* api = ssim_metric_api();
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

// Double-precision reference of §9.4.3 on code values: the window values of the valid region, textbook formula,
// exact Gaussian. Returns the (w-10) x (h-10) window values, row-major.
std::vector<double> reference_windows(const Pic& r, const Pic& d, size_t p) {
  const uint32_t w = r.planes.width[p];
  const uint32_t h = r.planes.height[p];
  const auto peak = static_cast<double>((1U << r.planes.bit_depth) - 1U);
  const double c1 = (0.01 * peak) * (0.01 * peak);
  const double c2 = (0.03 * peak) * (0.03 * peak);
  std::array<double, 11> g{};
  double total = 0;
  for (size_t i = 0; i < g.size(); ++i) {
    const double k = static_cast<double>(i) - 5.0;
    g[i] = std::exp(-(k * k) / (2.0 * 1.5 * 1.5));
    total += g[i];
  }
  for (auto& v : g) {
    v /= total;
  }
  std::vector<double> out;
  for (uint32_t y = 0; y + 11 <= h; ++y) {
    for (uint32_t x = 0; x + 11 <= w; ++x) {
      double mx = 0;
      double my = 0;
      double xx = 0;
      double yy = 0;
      double xy = 0;
      for (uint32_t j = 0; j < 11; ++j) {
        for (uint32_t i = 0; i < 11; ++i) {
          const double wt = g[j] * g[i];
          const double a = r.get(p, x + i, y + j);
          const double b = d.get(p, x + i, y + j);
          mx += wt * a;
          my += wt * b;
          xx += wt * a * a;
          yy += wt * b * b;
          xy += wt * a * b;
        }
      }
      const double sx = xx - (mx * mx);
      const double sy = yy - (my * my);
      const double sxy = xy - (mx * my);
      out.push_back(((2 * mx * my + c1) * (2 * sxy + c2)) / (((mx * mx) + (my * my) + c1) * (sx + sy + c2)));
    }
  }
  return out;
}

double mean(const std::vector<double>& v) {
  double s = 0;
  for (const double x : v) {
    s += x;
  }
  return s / static_cast<double>(v.size());
}

// The deterministic synthetic pair of tests/oracles/run_libvmaf_ssim.py (`synth_luma`), sample for sample.
uint32_t synth_hash(uint32_t x, uint32_t y, uint32_t f, uint32_t s) {
  uint32_t v = (x * 73856093U) ^ (y * 19349663U) ^ (f * 83492791U) ^ (s * 2654435769U);
  v *= 2246822519U;
  v ^= v >> 15U;
  v *= 3266489917U;
  v ^= v >> 13U;
  return v;
}

void fill_synth(Pic& pic, uint32_t frame, bool distorted) {
  const uint32_t w = pic.planes.width[0];
  const uint32_t h = pic.planes.height[0];
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      int64_t base = 40 + ((3 * x + 2 * y + 7 * frame) % 128) + (synth_hash(x, y, frame, 1) >> 27U);
      if (distorted) {
        const uint32_t cells = (((x / 4) + (y / 4) + frame) & 1U) * 6U;
        base += static_cast<int64_t>(synth_hash(x, y, frame, 2) >> 28U) - 8 + static_cast<int64_t>(cells);
      }
      if (pic.planes.bit_depth == 10) {
        base = (base * 4) + (synth_hash(x, y, frame, distorted ? 4 : 3) >> 30U);
      }
      pic.set(0, x, y, static_cast<uint32_t>(base));
    }
  }
  for (size_t p = 1; p < pic.planes.planes; ++p) {
    for (uint32_t y = 0; y < pic.planes.height[p]; ++y) {
      for (uint32_t x = 0; x < pic.planes.width[p]; ++x) {
        pic.set(p, x, y, pic.planes.bit_depth == 8 ? 128U : 512U);
      }
    }
  }
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

TEST_CASE("ssim descriptor and registration", "[metrics][ssim][fast]") {
  std::string err;
  const std::string id = "ssim";
  const auto bytes = ovt::metrics::validate_metric_api(ssim_metric_api(), &id, err);
  REQUIRE(bytes.has_value());
  const auto* d = flatbuffers::GetRoot<ovt::fb::MetricDescriptor>(bytes->data());
  CHECK(d->id()->str() == "ssim");
  CHECK(d->per_plane());
  CHECK(d->higher_is_better());
  CHECK(d->domain() == ovt::fb::WorkingDomain::CodeValue);
  CHECK(d->version()->str() == "ssim 1.0 border=libvmaf");
  CHECK(d->outputs()->Get(0)->str() == "y");
  ovt::metrics::PluginRegistry reg(ovt::metrics::PluginRegistry::Options{
      .ovt_metric_path = "", .ovt_plugin_path = "", .defaults = std::vector<std::string>{}});
  ovt::metrics::register_builtin_kernels(reg);
  CHECK(reg.find("ssim") != nullptr);
  CHECK(reg.find("psnr") != nullptr);
}

TEST_CASE("ssim rejects unknown options and bad inputs", "[metrics][ssim][fast]") {
  const auto* api = ssim_metric_api();
  ovt_Metric* m = nullptr;
  CHECK(api->create(R"({"nope":1})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"border":"replicate"})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"border":1})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"map_block":12})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"planes":["q"]})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"planes":[]})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create("not json", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"border":"valid","planes":["y","u"],"map_block":16})", &m) == OVT_OK);
  api->destroy(m);
  Instance inst;
  Pic a(32, 32, 1, 8);
  Pic b(48, 32, 1, 8);
  ovt_Buffer out{};
  CHECK(api->score(inst.m, &a.planes, &b.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  CHECK(api->score(inst.m, nullptr, &b.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  // A plane smaller than the 11 x 11 window cannot be scored.
  Pic tiny(10, 40, 0, 8);
  CHECK(api->score(inst.m, &tiny.planes, &tiny.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  // 4:0:0 input with ["u"] selects no plane.
  Instance only_u(R"({"planes":["u"]})");
  Pic gray(32, 32, 0, 8);
  CHECK(api->score(only_u.m, &gray.planes, &gray.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  // finish() yields no sequence score (pooling is WI-3.311).
  CHECK(api->finish(inst.m, &out) == OVT_OK);
  CHECK(out.data == nullptr);
}

TEST_CASE("ssim of identical pictures is 1 and the map is all 1", "[metrics][ssim][fast]") {
  CaptureSink sink;
  ovt::metrics::set_map_sink(&sink);
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      7);
  for (const char* border : {"libvmaf", "valid"}) {
    for (const uint32_t bits : {8U, 10U, 16U}) {
      Pic r(70, 52, 1, bits);
      r.fill_texture(rng, 3);
      const std::string opts = std::string(R"({"planes":["y","u","v"],"map_block":16,"border":")") + border + "\"}";
      Instance inst(opts.c_str());
      const Scored s = inst.score(r, r, 9);
      REQUIRE(s.values.size() == 3);
      for (const double v : s.values) {
        CHECK(std::fabs(v - 1.0) <= 1e-6);
      }
      const auto& blob = sink.maps.at("ssim/y/000009/16");
      REQUIRE(blob.size() == 32 + (5U * 4U * 4U));  // ceil(70/16) x ceil(52/16)
      CHECK(std::memcmp(blob.data(), "OVTMAP01", 8) == 0);
      for (size_t i = 0; i < 20; ++i) {
        const float v = map_value(blob, i);
        if (!std::isnan(v)) {
          CHECK(std::fabs(v - 1.0F) <= 1e-6F);
        }
      }
    }
  }
  ovt::metrics::set_map_sink(nullptr);
}

TEST_CASE("ssim valid border matches a double-precision reference", "[metrics][ssim][fast]") {
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      21);
  for (const uint32_t bits : {8U, 10U, 12U}) {
    Pic r(61, 47, 1, bits, 5);  // odd sizes, padded strides, chroma 31 x 24
    Pic d(61, 47, 1, bits, 2);
    r.fill_texture(rng, 0);
    d.fill_texture(rng, 11);
    Instance inst(R"({"border":"valid","planes":["y","u","v"]})");
    const Scored s = inst.score(r, d);
    for (size_t p = 0; p < 3; ++p) {
      CHECK(std::fabs(s.values[p] - mean(reference_windows(r, d, p))) <= 5e-5);
    }
  }
  // Without "planes" only luma is computed.
  Pic r(40, 40, 1, 8);
  Pic d(40, 40, 1, 8);
  r.fill_texture(rng, 0);
  d.fill_texture(rng, 5);
  Instance inst;
  const Scored s = inst.score(r, d);
  CHECK(std::isfinite(s.values[0]));
  CHECK(std::isnan(s.values[1]));
  CHECK(std::isnan(s.values[2]));
}

TEST_CASE("ssim libvmaf border below the down-scale threshold equals valid within the tap table error",
          "[metrics][ssim][fast]") {
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      33);
  Pic r(90, 70, 0, 8);
  Pic d(90, 70, 0, 8);
  r.fill_texture(rng, 0);
  d.fill_texture(rng, 9);
  const Scored a = Instance().score(r, d);
  const Scored b = Instance(R"({"border":"valid"})").score(r, d);
  // libvmaf's 1-D window is the Gaussian rounded to six decimals and sums to 1.000002; E[x^2] - mu^2 cancels, so the
  // error reaches 1.5e-4 on this noise image (docs/metrics_differences.md).
  CHECK(std::fabs(a.values[0] - b.values[0]) <= 3e-4);
}

TEST_CASE("ssim of a bit-depth-promoted pair is equal within 1e-6", "[metrics][ssim][fast]") {
  // WI-3.307 acceptance test 2: the 8-bit pair and the same pair x4 at 10 bits, distorted by a constant +1 / +4.
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      44);
  Pic r8(120, 80, 1, 8);
  r8.fill_texture(rng, 0);
  Pic d8(120, 80, 1, 8);
  Pic r10(120, 80, 1, 10);
  Pic d10(120, 80, 1, 10);
  for (size_t p = 0; p < 3; ++p) {
    for (uint32_t y = 0; y < r8.planes.height[p]; ++y) {
      for (uint32_t x = 0; x < r8.planes.width[p]; ++x) {
        const uint32_t v = r8.get(p, x, y);
        d8.set(p, x, y, v + 1U);
        r10.set(p, x, y, v * 4U);
        d10.set(p, x, y, (v * 4U) + 4U);
      }
    }
  }
  for (const char* border : {"libvmaf", "valid"}) {
    const std::string opts = std::string(R"({"planes":["y","u","v"],"border":")") + border + "\"}";
    Instance a(opts.c_str());
    Instance b(opts.c_str());
    const Scored s8 = a.score(r8, d8);
    const Scored s10 = b.score(r10, d10);
    for (size_t p = 0; p < 3; ++p) {
      CHECK(std::fabs(s8.values[p] - s10.values[p]) <= 1e-6);
      CHECK(s8.values[p] < 1.0);
    }
  }
}

TEST_CASE("ssim block map values are the mean of the window values centred in each block", "[metrics][ssim][fast]") {
  CaptureSink sink;
  ovt::metrics::set_map_sink(&sink);
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      55);
  Pic r(50, 44, 1, 8);
  Pic d(50, 44, 1, 8);
  r.fill_texture(rng, 0);
  d.fill_texture(rng, 4);
  Instance inst(R"({"border":"valid","planes":["y","u"],"map_block":8})");
  const Scored s = inst.score(r, d, 3);
  ovt::metrics::set_map_sink(nullptr);
  CHECK(s.map_blob_id == "ssim/y/000003/8");
  CHECK(s.map_block_size == 8);
  REQUIRE(sink.maps.size() == 2);
  const auto& blob = sink.maps.at("ssim/y/000003/8");
  const uint32_t bw = (50 + 7) / 8;
  const uint32_t bh = (44 + 7) / 8;
  REQUIRE(blob.size() == 32 + (static_cast<size_t>(bw) * bh * 4));
  CHECK(blob[18] == 0);  // component Y
  CHECK(blob[19] == 0);  // higher is better
  const auto win = reference_windows(r, d, 0);
  const uint32_t ow = 50 - 10;
  std::vector<double> sum(static_cast<size_t>(bw) * bh, 0.0);
  std::vector<int> cnt(sum.size(), 0);
  for (size_t i = 0; i < win.size(); ++i) {
    const uint32_t cx = static_cast<uint32_t>(i % ow) + 5;
    const uint32_t cy = static_cast<uint32_t>(i / ow) + 5;
    const size_t b = (static_cast<size_t>(cy / 8) * bw) + (cx / 8);
    sum[b] += win[i];
    ++cnt[b];
  }
  size_t empty = 0;
  for (size_t b = 0; b < sum.size(); ++b) {
    if (cnt[b] == 0) {  // an edge block no window is centred in (x >= 45 here): NaN
      CHECK(std::isnan(map_value(blob, b)));
      ++empty;
      continue;
    }
    CHECK(std::fabs(static_cast<double>(map_value(blob, b)) - (sum[b] / cnt[b])) <= 5e-5);
  }
  CHECK(empty == 12);  // centres span x 5..44, y 5..38: the last block column and the last block row are empty
  // The chroma map shares the grid: 4 x 4 chroma samples per 8 x 8 luma block.
  const auto& ublob = sink.maps.at("ssim/u/000003/8");
  CHECK(ublob.size() == blob.size());
  CHECK(ublob[18] == 1);
}

TEST_CASE("ssim scalar and Highway kernels are bit-identical", "[metrics][ssim][fast]") {
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      66);
  const std::array<float, 11> taps = {0.001028F, 0.007599F, 0.036001F, 0.109361F, 0.213006F, 0.266012F,
                                      0.213006F, 0.109361F, 0.036001F, 0.007599F, 0.001028F};
  for (int i = 0; i < 100; ++i) {
    const size_t n = 1 + (rng() % 200);  // odd lengths exercise the scalar tail of the vector loops
    std::vector<float> a(n + 10);
    std::vector<float> b(n + 10);
    for (size_t j = 0; j < a.size(); ++j) {
      a[j] = static_cast<float>(rng() % 256U);
      b[j] = static_cast<float>(rng() % 256U);
    }
    // products
    std::array<std::vector<float>, 6> p;
    for (auto& v : p) {
      v.assign(a.size(), 0.0F);
    }
    ovt::metrics::ssim_products_scalar(a.data(), b.data(), p[0].data(), p[1].data(), p[2].data(), a.size());
    ovt::metrics::ssim_products_hwy(a.data(), b.data(), p[3].data(), p[4].data(), p[5].data(), a.size());
    for (size_t k = 0; k < 3; ++k) {
      REQUIRE(std::memcmp(p[k].data(), p[k + 3].data(), a.size() * sizeof(float)) == 0);
    }
    // horizontal filter
    std::vector<float> h0(n);
    std::vector<float> h1(n);
    ovt::metrics::ssim_hfilter_scalar(a.data(), h0.data(), n, taps.data());
    ovt::metrics::ssim_hfilter_hwy(a.data(), h1.data(), n, taps.data());
    REQUIRE(std::memcmp(h0.data(), h1.data(), n * sizeof(float)) == 0);
    // vertical filter and window formula: five moments of 11 rows, built from filtered rows of random data
    std::array<std::vector<float>, 5> mom;
    for (auto& m : mom) {
      m.resize(11 * n);
    }
    for (size_t row = 0; row < 11; ++row) {
      for (size_t j = 0; j < n; ++j) {
        const auto x = static_cast<float>(rng() % 256U);
        const auto y = static_cast<float>(rng() % 256U);
        mom[0][(row * n) + j] = x;
        mom[1][(row * n) + j] = y;
        mom[2][(row * n) + j] = x * x;
        mom[3][(row * n) + j] = y * y;
        mom[4][(row * n) + j] = x * y;
      }
    }
    const std::array<const float*, 5> mp = {mom[0].data(), mom[1].data(), mom[2].data(), mom[3].data(), mom[4].data()};
    const ovt::metrics::SsimConstants k{6.5025F, 58.5225F};
    std::vector<float> w0(n);
    std::vector<float> w1(n);
    ovt::metrics::ssim_vwindow_scalar(mp.data(), n, n, taps.data(), k, w0.data());
    ovt::metrics::ssim_vwindow_hwy(mp.data(), n, n, taps.data(), k, w1.data());
    REQUIRE(std::memcmp(w0.data(), w1.data(), n * sizeof(float)) == 0);
  }
  // sample conversion, 8 and 16 bit, padded strides
  for (const uint32_t bytes : {1U, 2U}) {
    const uint32_t w = 77;
    const uint32_t h = 9;
    const uint32_t stride = (w * bytes) + 3;
    std::vector<uint8_t> src(static_cast<size_t>(stride) * h);
    for (auto& v : src) {
      v = static_cast<uint8_t>(rng());
    }
    std::vector<float> d0(static_cast<size_t>(w) * h);
    std::vector<float> d1(d0.size());
    ovt::metrics::ssim_to_float_scalar(src.data(), stride, w, h, bytes, 0.25F, d0.data(), w);
    ovt::metrics::ssim_to_float_hwy(src.data(), stride, w, h, bytes, 0.25F, d1.data(), w);
    CHECK(std::memcmp(d0.data(), d1.data(), d0.size() * sizeof(float)) == 0);
  }
}

TEST_CASE("ssim matches libvmaf float_ssim on the synthetic down-scaled pairs", "[metrics][ssim][golden][fast]") {
  // The pairs need libvmaf's f = 2 and f = 3 down-scaling (odd sizes included) and 10-bit input; the golden holds
  // libvmaf 3.0.0's float_ssim from tests/oracles/run_libvmaf_ssim.py synth.
  std::ifstream gf(std::filesystem::path(OVT_METRICS_GOLDEN_DIR) / "ssim_synth_libvmaf.json");
  REQUIRE(gf);
  const auto golden = nlohmann::json::parse(gf);
  for (const auto& c : golden.at("cases")) {
    const auto w = c.at("width").get<uint32_t>();
    const auto h = c.at("height").get<uint32_t>();
    const auto bits = c.at("bit_depth").get<uint32_t>();
    Pic r(w, h, 0, bits);
    Pic d(w, h, 0, bits);
    Instance inst;
    const auto& want = c.at("float_ssim");
    for (uint32_t f = 0; f < want.size(); ++f) {
      fill_synth(r, f, false);
      fill_synth(d, f, true);
      const Scored s = inst.score(r, d, static_cast<int32_t>(f));
      INFO(c.at("name").get<std::string>() << " frame " << f);
      CHECK(std::fabs(s.values[0] - want[f].get<double>()) <= 1e-4);
    }
  }
}

TEST_CASE("ssim matches libvmaf float_ssim on the Netflix pair", "[metrics][ssim][golden][needs-corpus]") {
  const auto ref_path = ovt::test::corpus_or_skip("netflix_src01_hrc00_576x324");
  const auto dist_path = ovt::test::corpus_or_skip("netflix_src01_hrc01_576x324");
  std::ifstream gf(std::filesystem::path(OVT_METRICS_GOLDEN_DIR) / "ssim_netflix_src01_576x324.json");
  REQUIRE(gf);
  const auto golden = nlohmann::json::parse(gf);
  const auto& want = golden.at("float_ssim");
  Y4m ry(ref_path);
  Y4m dy(dist_path);
  Pic r(ry.w_, ry.h_, 1, 8);
  Pic d(dy.w_, dy.h_, 1, 8);
  Instance inst;
  size_t n = 0;
  while (ry.next(r) && dy.next(d)) {
    REQUIRE(n < want.size());
    const Scored s = inst.score(r, d, static_cast<int32_t>(n));
    CHECK(std::fabs(s.values[0] - want[n].get<double>()) <= 1e-4);
    ++n;
  }
  CHECK(n == 48);
  CHECK(n == want.size());
}

// Hidden (run with "[bench]"): single-thread throughput of psnr + ssim on a 1080p 8-bit 4:2:0 pair, the quantity of
// the chapter 14 §14.9.3 budget bench_metrics_psnr_ssim_1080p (fps_4_threads >= 200 on m1-air-8gb; WI-0.560's
// benchmark binaries run the same calls on four threads). Prints the numbers for docs/acceptance/wi/WI-3.307.md.
TEST_CASE("ssim 1080p single-thread throughput", "[.][bench][metrics][ssim]") {
  Pic r(1920, 1080, 1, 8);
  Pic d(1920, 1080, 1, 8);
  std::mt19937 rng(  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds make the cases reproducible
      1);
  r.fill_texture(rng, 0);
  d.fill_texture(rng, 3);
  for (const char* border : {"libvmaf", "valid"}) {
    const std::string opts = std::string(R"({"border":")") + border + "\"}";
    Instance inst(opts.c_str());
    constexpr int kFrames = 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
      (void)inst.score(r, d, i);
    }
    const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
    std::printf("ssim 1080p luma, border=%s: %.1f fps (1 thread)\n", border, kFrames / dt.count());
  }
  SUCCEED();
}
