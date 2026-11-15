// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Map conservation of the psnr metric (chapter 9 §9.4.7, chapter 14 WI-3.590 acceptance test 3): with
// map_block = 16, the per-block MSE times block area, summed over the map, equals the frame MSE times the frame area.
//
// The psnr map stores the PSNR of each block as float32, clipped to psnr_max (§9.4.7, #385), so the block MSE can be
// read back from the map only to float32 precision and only for blocks below psnr_max. The test therefore checks
// conservation twice:
//   1. exactly (1e-9 relative): the block grid of the map header, with chroma blocks covering the same area and edge
//      blocks holding the samples that exist, partitions every plane; the block SSEs over that grid sum to the frame
//      SSE that the FrameScore's mse_* report; and every map value is the PSNR of its block's MSE (to float32
//      rounding), so the map describes exactly that partition;
//   2. from the map values alone, on content where no block reaches psnr_max: sum of peak^2 / 10^(PSNR/10) times
//      the block area equals frame MSE times frame area within 1e-6 relative, the float32 limit.
// SPEC-ASSUMPTION(#566): acceptance test 3's 1e-9 applies to check 1; check 2 is bounded by float32.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "metrics/kernels/map_sink.h"
#include "metrics/kernels/psnr_kernels.h"
#include "metrics/kernels/psnr_metric.h"
#include "metrics_golden_suite.h"

namespace {

namespace suite = ovt::test::metrics_suite;

class CaptureSink final : public ovt::metrics::MapSink {
 public:
  void put(const std::string& id, std::vector<uint8_t> bytes) override {
    const std::lock_guard lock(mu_);
    maps_[id] = std::move(bytes);
  }
  [[nodiscard]] std::vector<uint8_t> take(const std::string& id) {
    const std::lock_guard lock(mu_);
    const auto it = maps_.find(id);
    REQUIRE(it != maps_.end());
    auto bytes = std::move(it->second);
    maps_.erase(it);
    return bytes;
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::vector<uint8_t>> maps_;
};

// Installs the sink for the lifetime of the scope.
class SinkScope {
 public:
  explicit SinkScope(ovt::metrics::MapSink& sink) { ovt::metrics::set_map_sink(&sink); }
  SinkScope(const SinkScope&) = delete;
  SinkScope& operator=(const SinkScope&) = delete;
  SinkScope(SinkScope&&) = delete;
  SinkScope& operator=(SinkScope&&) = delete;
  ~SinkScope() { ovt::metrics::set_map_sink(nullptr); }
};

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(b.at(at + i)) << (8U * i);
  }
  return v;
}

// An .ovtmap image (§9.4.7 header) decoded.
struct OvtMap {
  uint32_t blocks_w = 0;
  uint32_t blocks_h = 0;
  uint32_t block_size = 0;
  uint32_t component = 0;
  std::vector<float> values;
};

OvtMap parse_ovtmap(const std::vector<uint8_t>& blob) {
  REQUIRE(blob.size() >= ovt::metrics::kOvtMapHeaderSize);
  REQUIRE(std::memcmp(blob.data(), "OVTMAP01", 8) == 0);
  OvtMap m;
  m.blocks_w = le32(blob, 8);
  m.blocks_h = le32(blob, 12);
  m.block_size = static_cast<uint32_t>(blob.at(16)) | (static_cast<uint32_t>(blob.at(17)) << 8U);
  m.component = blob.at(18);
  const size_t n = static_cast<size_t>(m.blocks_w) * m.blocks_h;
  REQUIRE(blob.size() == ovt::metrics::kOvtMapHeaderSize + (n * 4));
  m.values.resize(n);
  std::memcpy(m.values.data(), blob.data() + ovt::metrics::kOvtMapHeaderSize, n * 4);
  return m;
}

std::string map_id(size_t plane, int32_t frame, uint32_t block) {
  static constexpr std::array<const char*, 3> kNames = {"y", "u", "v"};
  std::array<char, 16> num{};
  std::snprintf(num.data(), num.size(), "%06d", frame);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  return std::string("psnr/") + kNames.at(plane) + "/" + num.data() + "/" + std::to_string(block);
}

struct Conservation {
  double frame_mse_area = 0.0;      // FrameScore mse_p x plane area
  double block_sse_sum = 0.0;       // check 1: sum over the grid of block MSE x block area
  double map_mse_area = 0.0;        // check 2: sum over the map of MSE read back from the PSNR x block area
  uint64_t covered = 0;             // samples inside the grid's blocks
  uint32_t clipped = 0;             // blocks at psnr_max
  double max_value_error_db = 0.0;  // |map value - PSNR(block MSE)|
};

// Both checks for plane p of one scored frame.
Conservation conserve(const suite::Picture& r, const suite::Picture& d, size_t p, const OvtMap& map, double frame_mse) {
  const ovt_MetricPlanes& rp = r.planes;
  const ovt_MetricPlanes& dp = d.planes;
  const uint32_t block = map.block_size;
  REQUIRE(map.component == p);
  REQUIRE(map.blocks_w == (rp.width[0] + block - 1) / block);
  REQUIRE(map.blocks_h == (rp.height[0] + block - 1) / block);
  const uint32_t bw = rp.width[0] > rp.width[p] ? block / 2 : block;
  const uint32_t bh = rp.height[0] > rp.height[p] ? block / 2 : block;
  const double peak = std::ldexp(1.0, rp.bit_depth) - 1.0;
  const double psnr_max = 60.0 + (6.0 * (rp.bit_depth - 8.0));
  Conservation c;
  c.frame_mse_area = frame_mse * static_cast<double>(rp.width[p]) * rp.height[p];
  for (uint32_t by = 0; by < map.blocks_h; ++by) {
    for (uint32_t bx = 0; bx < map.blocks_w; ++bx) {
      const uint32_t x0 = bx * bw;
      const uint32_t y0 = by * bh;
      const uint32_t cols = x0 < rp.width[p] ? std::min(bw, rp.width[p] - x0) : 0;
      const uint32_t rows = y0 < rp.height[p] ? std::min(bh, rp.height[p] - y0) : 0;
      const float v = map.values.at((static_cast<size_t>(by) * map.blocks_w) + bx);
      if (cols == 0 || rows == 0) {
        CHECK(std::isnan(v));  // no sample of this plane in the block
        continue;
      }
      const auto ro =
          (static_cast<ptrdiff_t>(y0) * rp.stride_bytes[p]) + (static_cast<ptrdiff_t>(x0) * rp.sample_bytes);
      const auto dof =
          (static_cast<ptrdiff_t>(y0) * dp.stride_bytes[p]) + (static_cast<ptrdiff_t>(x0) * dp.sample_bytes);
      const uint64_t sse = ovt::metrics::psnr_sse_scalar(rp.data[p] + ro, rp.stride_bytes[p], dp.data[p] + dof,
                                                         dp.stride_bytes[p], cols, rows, rp.sample_bytes, rp.bit_depth);
      const double area = static_cast<double>(cols) * rows;
      const double mse = static_cast<double>(sse) / area;
      c.covered += static_cast<uint64_t>(cols) * rows;
      c.block_sse_sum += mse * area;
      const double want = mse > 0.0 ? std::min(10.0 * std::log10(peak * peak / mse), psnr_max) : psnr_max;
      c.max_value_error_db = std::max(c.max_value_error_db, std::fabs(static_cast<double>(v) - want));
      if (static_cast<double>(v) >= psnr_max) {
        ++c.clipped;
      }
      c.map_mse_area += peak * peak / std::pow(10.0, static_cast<double>(v) / 10.0) * area;
    }
  }
  return c;
}

double rel(double a, double b) {
  return std::fabs(a - b) / std::max(std::fabs(b), 1e-300);
}

// Scores one frame with map_block 16 and runs both checks on every plane. `expect_unclipped`: content on which no
// block may reach psnr_max, so check 2 applies.
void check_frame(const suite::Metric& inst, CaptureSink& sink, const suite::Picture& r, const suite::Picture& d,
                 int32_t frame, bool expect_unclipped) {
  const suite::Score s = inst.score(r.planes, d.planes, frame);
  REQUIRE(s.map_block_size == 16);
  REQUIRE(s.map_blob_id == map_id(0, frame, 16));
  for (size_t p = 0; p < 3; ++p) {
    const OvtMap map = parse_ovtmap(sink.take(map_id(p, frame, 16)));
    const Conservation c = conserve(r, d, p, map, s.values.at(5 + p));
    INFO("frame " << frame << " plane " << p);
    CHECK(c.covered == static_cast<uint64_t>(r.planes.width[p]) * r.planes.height[p]);
    CHECK(rel(c.block_sse_sum, c.frame_mse_area) <= 1e-9);
    CHECK(c.max_value_error_db <= 1e-5);  // float32 of a PSNR below 108 dB: half an ulp is under 4e-6
    if (expect_unclipped) {
      REQUIRE(c.clipped == 0);
      CHECK(rel(c.map_mse_area, c.frame_mse_area) <= 1e-6);
    }
  }
}

// Deterministic content: a gradient with xorshift noise; `noise` is the noise amplitude in code values.
void fill(suite::Picture& pic, uint64_t seed, uint32_t noise) {
  uint64_t s = seed;
  const uint32_t maxv = (1U << pic.planes.bit_depth) - 1U;
  for (size_t p = 0; p < 3; ++p) {
    for (uint32_t y = 0; y < pic.planes.height[p]; ++y) {
      for (uint32_t x = 0; x < pic.planes.width[p]; ++x) {
        s ^= s << 13U;
        s ^= s >> 7U;
        s ^= s << 17U;
        const uint32_t base = ((x * 3U) + (y * 5U) + (static_cast<uint32_t>(p) * 40U)) % (maxv + 1U);
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

}  // namespace

TEST_CASE("psnr maps conserve MSE x area over the block grid", "[metrics][psnr][maps][fast]") {
  CaptureSink sink;
  const SinkScope scope(sink);
  // Sizes that divide by 16, that leave partial edge blocks, and odd sizes whose chroma planes round up.
  struct Case {
    uint32_t w, h, bits;
  };
  int32_t frame = 0;
  for (const Case c : {Case{576, 324, 8}, Case{333, 197, 8}, Case{64, 48, 10}, Case{97, 31, 10}, Case{17, 17, 12}}) {
    suite::Picture r(c.w, c.h, c.bits);
    suite::Picture d(c.w, c.h, c.bits);
    fill(r, 0x1234U + static_cast<uint64_t>(frame), 0);
    fill(d, 0x9876U + static_cast<uint64_t>(frame),
         (1U << c.bits) / 4U);  // every block far from the reference: no clipping
    INFO(c.w << "x" << c.h << " " << c.bits << "-bit");
    const suite::Metric inst(ovt::metrics::psnr_metric_api(), R"({"map_block":16})");  // one format per instance
    check_frame(inst, sink, r, d, frame++, true);
    // Mostly identical content: many blocks at psnr_max, so only the exact check applies.
    suite::Picture near(c.w, c.h, c.bits);
    near.buf = r.buf;
    near.planes.bit_depth = r.planes.bit_depth;
    for (size_t p = 0; p < 3; ++p) {
      near.planes.data[p] = near.buf.at(p).data();
    }
    near.buf[0][0] ^= 1U;
    check_frame(inst, sink, r, near, frame++, false);
  }
}

TEST_CASE("psnr maps conserve MSE x area on the Netflix pair", "[metrics][psnr][maps][golden][needs-corpus]") {
  CaptureSink sink;
  const SinkScope scope(sink);
  suite::Metric inst(ovt::metrics::psnr_metric_api(), R"({"map_block":16})");
  const uint32_t n =
      suite::for_each_frame(suite::kPairs[0], [&](uint32_t i, const suite::Picture& r, const suite::Picture& d) {
        check_frame(inst, sink, r, d, static_cast<int32_t>(i), false);
      });
  CHECK(n == 48);
}
