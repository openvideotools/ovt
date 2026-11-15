// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Cross-architecture tolerance rule of the own kernels (chapter 9 §9.4.1; chapter 14 WI-3.590 acceptance test 4).
// core/tests/metrics/golden/xarch_kernels.json holds psnr, ssim_x264 and ssim results on deterministic pictures,
// recorded on x86_64 with the AVX2 target. This test recomputes them with every Highway target the build generated
// and the CPU supports (x86_64: SSE4, AVX2, AVX3, ...; arm64: NEON, SVE, ...) and with the scalar reference kernels,
// and requires:
//   * integer outputs bit-identical: the psnr SSE sums per plane (equivalently mse_* x samples), and ssim_x264, whose
//     only float operations are IEEE divisions and float sums in a fixed order (§9.4.4);
//   * psnr in dB within 1e-12 relative (a log10 of the identical integer sum; libm may differ in the last ulp);
//   * the float kernel `ssim` within 1e-4 relative of the golden (across ISAs, §9.4.1), and within 1e-9 relative
//     between the targets and the scalar reference on one host (WI-3.307 makes them bit-identical in fact).
// Regenerate on an x86_64 machine with AVX2: OVT_UPDATE_GOLDEN=1 ovt_tests_metrics "[xarch]".
//
// The production build compiles no Highway scalar target (HWY_TARGETS excludes superseded baselines), so "the
// scalar build" of acceptance test 4 is the *_scalar.cpp reference: psnr_sse_scalar for the SSE sums, and the ssim
// building blocks run in a plain loop below. ssim_x264 has no SIMD path; its target runs check that the dispatch
// state does not change it. There is no `ms_ssim` kernel yet (WI-3.311 adds it, with its own cross-ISA golden).

#include <hwy/targets.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "metrics/kernels/psnr_kernels.h"
#include "metrics/kernels/psnr_metric.h"
#include "metrics/kernels/ssim_kernels.h"
#include "metrics/kernels/ssim_metric.h"
#include "metrics/kernels/ssim_x264.h"
#include "metrics_golden_suite.hpp"
#include "test_support/golden.h"

namespace {

namespace suite = ovt::test::metrics_suite;

struct Case {
  const char* name;
  uint32_t width;
  uint32_t height;
  uint32_t bits;
};

// Sizes with full and partial 8x8 / 16x16 blocks, odd sizes, x264's integer (<= 9 bit) and float branches, and one
// luma plane large enough for libvmaf's down-scale (f = round(520 / 256) = 2).
constexpr std::array<Case, 5> kCases = {{
    {"8bit_576x324", 576, 324, 8},
    {"8bit_333x197", 333, 197, 8},
    {"10bit_330x194", 330, 194, 10},
    {"10bit_600x520", 600, 520, 10},
    {"12bit_97x45", 97, 45, 12},
}};

// Gradient plus xorshift64 noise of amplitude `noise`: the same bytes on every platform.
void fill(suite::Picture& pic, uint64_t seed, uint32_t noise) {
  uint64_t s = seed;
  const uint32_t maxv = (1U << pic.planes.bit_depth) - 1U;
  for (size_t p = 0; p < 3; ++p) {
    for (uint32_t y = 0; y < pic.planes.height[p]; ++y) {
      for (uint32_t x = 0; x < pic.planes.width[p]; ++x) {
        s ^= s << 13U;
        s ^= s >> 7U;
        s ^= s << 17U;
        const uint32_t base = (((x * 7U) + (y * 3U)) << (pic.planes.bit_depth - 8U)) % (maxv + 1U);
        const uint32_t v = std::min(maxv, base + static_cast<uint32_t>(s % (noise + 1U)));
        const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(pic.planes.stride_bytes[p])) +
                           (static_cast<size_t>(x) * pic.planes.sample_bytes);
        auto& b = pic.buf.at(p);
        b[off] = static_cast<uint8_t>(v & 0xFFU);
        if (pic.planes.sample_bytes == 2) {
          b[off + 1] = static_cast<uint8_t>(v >> 8U);
        }
      }
    }
  }
}

struct Results {
  std::array<uint64_t, 3> psnr_sse{};
  std::array<double, 3> psnr_db{};
  std::array<double, 3> ssim_x264{};
  std::array<double, 3> ssim{};
};

// The three metrics through their ovt_metric_v1 tables (the dispatching Highway kernels).
Results run_metrics(const suite::Picture& r, const suite::Picture& d) {
  Results out;
  const suite::Metric psnr(ovt::metrics::psnr_metric_api(), "{}");
  const suite::Score sp = psnr.score(r.planes, d.planes, 0);
  const suite::Metric x264(ovt::metrics::ssim_x264_metric_api(), R"({"planes":["y","u","v"]})");
  const suite::Score sx = x264.score(r.planes, d.planes, 0);
  const suite::Metric ssim(ovt::metrics::ssim_metric_api(), R"({"planes":["y","u","v"]})");
  const suite::Score ss = ssim.score(r.planes, d.planes, 0);
  for (size_t p = 0; p < 3; ++p) {
    const double samples = static_cast<double>(r.planes.width[p]) * r.planes.height[p];
    // mse = SSE / samples in double; the SSE (< 2^53 here) comes back exactly.
    out.psnr_sse.at(p) = static_cast<uint64_t>(std::llround(sp.values.at(5 + p) * samples));
    out.psnr_db.at(p) = sp.values.at(p);
    out.ssim_x264.at(p) = sx.values.at(p);
    out.ssim.at(p) = ss.values.at(p);
  }
  return out;
}

// libvmaf 3.0's 11-tap table (iqa/ssim.c g_gaussian_window_h), as the ssim metric uses it for border "libvmaf".
constexpr std::array<float, ovt::metrics::kSsimTaps> kLibvmafTaps = {0.001028F, 0.007599F, 0.036001F, 0.109361F,
                                                                     0.213006F, 0.266012F, 0.213006F, 0.109361F,
                                                                     0.036001F, 0.007599F, 0.001028F};

// The ssim of plane p with the scalar building blocks, for planes libvmaf does not down-scale (min(W, H) < 384):
// samples scaled to the 8-bit range, products, horizontal then vertical 11-tap filters, the window formula, and the
// double mean in raster order, exactly the operations of ssim_metric.cpp.
double ssim_scalar_plane(const suite::Picture& r, const suite::Picture& d, size_t p) {
  using namespace ovt::metrics;  // NOLINT(google-build-using-namespace): the kernel names read better unqualified
  const uint32_t w = r.planes.width[p];
  const uint32_t h = r.planes.height[p];
  const float scale = 1.0F / static_cast<float>(1U << (r.planes.bit_depth - 8U));
  std::vector<float> a(static_cast<size_t>(w) * h);
  std::vector<float> b(a.size());
  ssim_to_float_scalar(r.planes.data[p], r.planes.stride_bytes[p], w, h, r.planes.sample_bytes, scale, a.data(), w);
  ssim_to_float_scalar(d.planes.data[p], d.planes.stride_bytes[p], w, h, d.planes.sample_bytes, scale, b.data(), w);
  const uint32_t ow = w - kSsimMargin;
  const uint32_t oh = h - kSsimMargin;
  std::array<std::vector<float>, 5> mom;
  for (auto& m : mom) {
    m.resize(static_cast<size_t>(ow) * h);
  }
  std::vector<float> aa(w);
  std::vector<float> bb(w);
  std::vector<float> ab(w);
  for (uint32_t y = 0; y < h; ++y) {
    const float* ra = a.data() + (static_cast<size_t>(y) * w);
    const float* rb = b.data() + (static_cast<size_t>(y) * w);
    ssim_products_scalar(ra, rb, aa.data(), bb.data(), ab.data(), w);
    const size_t at = static_cast<size_t>(y) * ow;
    ssim_hfilter_scalar(ra, mom[0].data() + at, ow, kLibvmafTaps.data());
    ssim_hfilter_scalar(rb, mom[1].data() + at, ow, kLibvmafTaps.data());
    ssim_hfilter_scalar(aa.data(), mom[2].data() + at, ow, kLibvmafTaps.data());
    ssim_hfilter_scalar(bb.data(), mom[3].data() + at, ow, kLibvmafTaps.data());
    ssim_hfilter_scalar(ab.data(), mom[4].data() + at, ow, kLibvmafTaps.data());
  }
  const float k1l = 0.01F * 255.0F;
  const float k2l = 0.03F * 255.0F;
  const SsimConstants k{k1l * k1l, k2l * k2l};
  std::vector<float> row(ow);
  double total = 0.0;
  for (uint32_t y = 0; y < oh; ++y) {
    const size_t at = static_cast<size_t>(y) * ow;
    const std::array<const float*, 5> rows = {mom[0].data() + at, mom[1].data() + at, mom[2].data() + at,
                                              mom[3].data() + at, mom[4].data() + at};
    ssim_vwindow_scalar(rows.data(), ow, ow, kLibvmafTaps.data(), k, row.data());
    for (const float v : row) {
      total += static_cast<double>(v);
    }
  }
  return total / (static_cast<double>(ow) * static_cast<double>(oh));
}

double rel(double a, double b) {
  return std::fabs(a - b) / std::max(std::fabs(b), 1e-300);
}

std::filesystem::path golden_path() {
  return std::filesystem::path(OVT_METRICS_GOLDEN_DIR) / "xarch_kernels.json";
}

// Restores the CPU's own target set when the test ends, also on a failed REQUIRE.
class TargetScope {
 public:
  TargetScope() = default;
  TargetScope(const TargetScope&) = delete;
  TargetScope& operator=(const TargetScope&) = delete;
  TargetScope(TargetScope&&) = delete;
  TargetScope& operator=(TargetScope&&) = delete;
  ~TargetScope() { hwy::SetSupportedTargetsForTest(0); }
  static void use(int64_t target) { hwy::SetSupportedTargetsForTest(target); }
};

void record_golden() {
  const auto targets = hwy::SupportedAndGeneratedTargets();
  if (std::find(targets.begin(), targets.end(), HWY_AVX2) == targets.end()) {
    FAIL("xarch_kernels.json is recorded with the AVX2 target: run OVT_UPDATE_GOLDEN=1 on an x86_64 AVX2 machine");
  }
  const TargetScope scope;
  TargetScope::use(HWY_AVX2);
  nlohmann::json cases = nlohmann::json::array();
  for (const Case& c : kCases) {
    suite::Picture r(c.width, c.height, c.bits);
    suite::Picture d(c.width, c.height, c.bits);
    fill(r, 0xC0FFEEULL + c.width, 1U << (c.bits - 6U));
    fill(d, 0xBADC0DEULL + c.height, 1U << (c.bits - 3U));
    const Results res = run_metrics(r, d);
    cases.push_back({{"name", c.name},
                     {"psnr_sse", res.psnr_sse},
                     {"psnr_db", res.psnr_db},
                     {"ssim_x264", res.ssim_x264},
                     {"ssim", res.ssim}});
  }
  const nlohmann::json doc = {{"recorded_on", "x86_64, Highway target AVX2"},
                              {"source", "core/tests/metrics/test_metrics_xarch.cpp (WI-3.590)"},
                              {"cases", cases}};
  CHECK(ovt::test::compare_or_update(golden_path(), doc.dump(1) + "\n"));
}

}  // namespace

TEST_CASE("xarch: psnr, ssim_x264 and ssim agree with the x86_64 AVX2 golden on every target",
          "[metrics][xarch][golden][fast]") {
  if (ovt::test::update_golden()) {
    record_golden();
    return;
  }
  std::ifstream gf(golden_path());
  REQUIRE(gf);
  const auto golden = nlohmann::json::parse(gf);
  REQUIRE(golden.at("cases").size() == kCases.size());
  const auto targets = hwy::SupportedAndGeneratedTargets();
  REQUIRE(!targets.empty());
  const TargetScope scope;
  for (size_t ci = 0; ci < kCases.size(); ++ci) {
    const Case& c = kCases.at(ci);
    const auto& g = golden.at("cases").at(ci);
    REQUIRE(g.at("name").get<std::string>() == c.name);
    suite::Picture r(c.width, c.height, c.bits);
    suite::Picture d(c.width, c.height, c.bits);
    fill(r, 0xC0FFEEULL + c.width, 1U << (c.bits - 6U));
    fill(d, 0xBADC0DEULL + c.height, 1U << (c.bits - 3U));

    // The scalar reference on this host.
    Results scalar;
    for (size_t p = 0; p < 3; ++p) {
      scalar.psnr_sse.at(p) = ovt::metrics::psnr_sse_scalar(
          r.planes.data[p], r.planes.stride_bytes[p], d.planes.data[p], d.planes.stride_bytes[p], r.planes.width[p],
          r.planes.height[p], r.planes.sample_bytes, r.planes.bit_depth);
      INFO(c.name << " scalar plane " << p);
      CHECK(scalar.psnr_sse.at(p) == g.at("psnr_sse").at(p).get<uint64_t>());
      if (std::min(r.planes.width[p], r.planes.height[p]) < 384) {  // libvmaf's f == 1
        scalar.ssim.at(p) = ssim_scalar_plane(r, d, p);
        CHECK(rel(scalar.ssim.at(p), g.at("ssim").at(p).get<double>()) <= 1e-4);
      }
    }

    for (const int64_t target : targets) {
      TargetScope::use(target);
      const Results got = run_metrics(r, d);
      for (size_t p = 0; p < 3; ++p) {
        INFO(c.name << " target " << hwy::TargetName(target) << " plane " << p);
        CHECK(got.psnr_sse.at(p) == g.at("psnr_sse").at(p).get<uint64_t>());
        CHECK(got.psnr_sse.at(p) == scalar.psnr_sse.at(p));
        CHECK(rel(got.psnr_db.at(p), g.at("psnr_db").at(p).get<double>()) <= 1e-12);
        CHECK(got.ssim_x264.at(p) == g.at("ssim_x264").at(p).get<double>());
        CHECK(rel(got.ssim.at(p), g.at("ssim").at(p).get<double>()) <= 1e-4);
        if (scalar.ssim.at(p) != 0.0) {
          CHECK(rel(got.ssim.at(p), scalar.ssim.at(p)) <= 1e-9);
        }
      }
    }
  }
}
