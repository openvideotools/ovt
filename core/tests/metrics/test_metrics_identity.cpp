// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the identity metric (chapter 9 §9.4.6, §9.4.7; WI-3.308 acceptance test 3): per-plane fractions, the
// sequence verdict of finish(), VQ-DIFF-0001, odd strides, 4:0:0, 10-bit, and the maps. The 1080p bit-flip case runs
// on the corpus entry syn_metrics_1080p_bitflip_f37.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "metrics/host/plugin_registry.h"
#include "metrics/kernels/diag_sink.h"
#include "metrics/kernels/identity.h"
#include "metrics/kernels/map_sink.h"
#include "metrics_test_pic.hpp"
#include "ovt_metrics_generated.h"
#include "test_support/corpus.h"

namespace {

using ovt::metrics::identity_metric_api;
using ovt::test::CaptureSink;
using ovt::test::decode_score;
using ovt::test::Pic;
using ovt::test::Scored;

class CaptureDiag final : public ovt::metrics::DiagSink {
 public:
  void report(const ovt::metrics::KernelDiagnostic& d) override {
    const std::lock_guard lock(mu);
    entries.push_back({std::string(d.metric_id), d.severity, std::string(d.code), d.message});
  }
  struct Entry {
    std::string metric_id;
    uint8_t severity;
    std::string code;
    std::string message;
  };
  std::mutex mu;
  std::vector<Entry> entries;
};

// Installs a CaptureDiag for the life of the object.
struct DiagScope {
  CaptureDiag diag;
  DiagScope() { ovt::metrics::set_diag_sink(&diag); }
  DiagScope(const DiagScope&) = delete;
  DiagScope& operator=(const DiagScope&) = delete;
  ~DiagScope() { ovt::metrics::set_diag_sink(nullptr); }
};

struct Instance {
  const ovt_MetricApi* api = identity_metric_api();
  ovt_Metric* m = nullptr;
  explicit Instance(const char* options = "{}") { REQUIRE(api->create(options, &m) == OVT_OK); }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  ~Instance() { api->destroy(m); }
  [[nodiscard]] Scored score(const Pic& r, const Pic& d, int32_t display = 0) const {
    ovt_Buffer out{};
    REQUIRE(api->score(m, &r.planes, &d.planes, ovt_FrameId{display, display}, &out) == OVT_OK);
    REQUIRE(out.data != nullptr);
    return decode_score(out);
  }
  // finish() output: the sequence values, and whether the score carries a per-frame record (it must not).
  [[nodiscard]] Scored finish(bool* has_frame = nullptr) const {
    ovt_Buffer out{};
    REQUIRE(api->finish(m, &out) == OVT_OK);
    REQUIRE(out.data != nullptr);
    if (has_frame != nullptr) {
      *has_frame = flatbuffers::GetRoot<ovt::fb::FrameScore>(out.data)->frame() != nullptr;
    }
    return decode_score(out);
  }
};

}  // namespace

TEST_CASE("identity descriptor and options", "[metrics][identity][fast]") {
  std::string err;
  const std::string id = "identity";
  const auto bytes = ovt::metrics::validate_metric_api(identity_metric_api(), &id, err);
  REQUIRE(bytes.has_value());
  const auto* d = flatbuffers::GetRoot<ovt::fb::MetricDescriptor>(bytes->data());
  CHECK(d->id()->str() == "identity");
  CHECK(d->higher_is_better());
  CHECK(d->range_min() == 0.0);
  CHECK(d->range_max() == 1.0);
  REQUIRE(d->outputs()->size() == 5);
  CHECK(d->outputs()->Get(3)->str() == "all");
  const auto* api = identity_metric_api();
  ovt_Metric* m = nullptr;
  CHECK(api->create(R"({"planes":["y"]})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"map_block":7})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create("[]", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"map_block":8})", &m) == OVT_OK);
  api->destroy(m);
  Instance inst;
  Pic a(32, 32, 1, 8);
  Pic b(32, 34, 1, 8);
  ovt_Buffer out{};
  CHECK(api->score(inst.m, &a.planes, &b.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
  CHECK(api->score(inst.m, nullptr, &b.planes, ovt_FrameId{0, 0}, &out) == OVT_E_INVALID_ARG);
}

TEST_CASE("identity of an identical pair is 1, the sequence verdict is 1", "[metrics][identity][fast]") {
  for (const uint32_t bits : {8U, 10U}) {
    DiagScope diag;
    Pic r(70, 38, 1, bits, 3);  // odd row padding must not matter
    Pic d(70, 38, 1, bits, 9);  // and neither may a different one
    Instance inst;
    for (int32_t f = 0; f < 3; ++f) {
      ovt::test::fill_synth(r, static_cast<uint32_t>(f), false);
      for (size_t p = 0; p < 3; ++p) {
        for (uint32_t y = 0; y < r.planes.height[p]; ++y) {
          for (uint32_t x = 0; x < r.planes.width[p]; ++x) {
            d.set(p, x, y, r.get(p, x, y));
          }
        }
      }
      const Scored s = inst.score(r, d, f);
      REQUIRE(s.values.size() == 4);
      CHECK(s.values == std::vector<double>{1.0, 1.0, 1.0, 1.0});
    }
    bool has_frame = true;
    const Scored fin = inst.finish(&has_frame);
    CHECK(!has_frame);
    CHECK(fin.values == std::vector<double>{3.0, 3.0, 1.0});
    CHECK(diag.diag.entries.empty());
  }
}

TEST_CASE("identity counts differing samples and reports the first one once", "[metrics][identity][fast]") {
  DiagScope diag;
  Pic r(40, 30, 1, 8, 2);
  ovt::test::fill_synth(r, 0, false);
  Instance inst;
  // Frame 0 is identical; frame 1 differs in one U sample and (later in raster order) two Y samples; frame 2 in Y.
  Pic d(40, 30, 1, 8, 2);
  d.buf = r.buf;
  CHECK(inst.score(r, d, 0).values == std::vector<double>{1.0, 1.0, 1.0, 1.0});
  d.set(1, 7, 3, r.get(1, 7, 3) ^ 1U);
  d.set(0, 30, 20, r.get(0, 30, 20) + 1U);
  d.set(0, 2, 25, r.get(0, 2, 25) + 5U);
  const Scored s1 = inst.score(r, d, 1);
  const double n_y = 40.0 * 30.0;
  const double n_c = 20.0 * 15.0;
  CHECK(s1.values[0] == (n_y - 2.0) / n_y);
  CHECK(s1.values[1] == (n_c - 1.0) / n_c);
  CHECK(s1.values[2] == 1.0);
  CHECK(s1.values[3] == ((n_y + (2.0 * n_c)) - 3.0) / (n_y + (2.0 * n_c)));
  // The first differing sample is the first in plane order (Y before U) and raster order: Y (30, 20) comes before
  // Y (2, 25) because its row is earlier.
  REQUIRE(diag.diag.entries.size() == 1);
  CHECK(diag.diag.entries[0].code == "VQ-DIFF-0001");
  CHECK(diag.diag.entries[0].metric_id == "identity");
  CHECK(diag.diag.entries[0].severity == 0);
  CHECK(diag.diag.entries[0].message == "first differing sample: frame 1, plane 0, (30,20): ref " +
                                            std::to_string(r.get(0, 30, 20)) + " dist " +
                                            std::to_string(r.get(0, 30, 20) + 1U));
  d.buf = r.buf;
  d.set(0, 0, 0, r.get(0, 0, 0) + 1U);
  (void)inst.score(r, d, 2);
  CHECK(diag.diag.entries.size() == 1);  // reported once per instance
  const Scored fin = inst.finish();
  CHECK(fin.values == std::vector<double>{1.0, 3.0, 0.0});
}

TEST_CASE("identity reports 10-bit samples and a flip in the high byte", "[metrics][identity][fast]") {
  DiagScope diag;
  Pic r(33, 17, 3, 10);
  ovt::test::fill_synth(r, 0, false);
  Pic d(33, 17, 3, 10);
  d.buf = r.buf;
  d.set(2, 32, 16, r.get(2, 32, 16) ^ 0x100U);
  Instance inst;
  const Scored s = inst.score(r, d, 9);
  CHECK(s.values[2] == (33.0 * 17.0 - 1.0) / (33.0 * 17.0));
  REQUIRE(diag.diag.entries.size() == 1);
  CHECK(diag.diag.entries[0].message.starts_with("first differing sample: frame 9, plane 2, (32,16): ref "));
}

TEST_CASE("identity on 4:0:0 input leaves u and v NaN", "[metrics][identity][fast]") {
  Pic r(20, 20, 0, 8);
  ovt::test::fill_synth(r, 0, false);
  Instance inst;
  const Scored s = inst.score(r, r);
  CHECK(s.values[0] == 1.0);
  CHECK(std::isnan(s.values[1]));
  CHECK(std::isnan(s.values[2]));
  CHECK(s.values[3] == 1.0);
}

TEST_CASE("identity finish without frames returns no score", "[metrics][identity][fast]") {
  Instance inst;
  ovt_Buffer out{};
  CHECK(inst.api->finish(inst.m, &out) == OVT_OK);
  CHECK(out.data == nullptr);
}

TEST_CASE("identity maps hold the fraction identical per block", "[metrics][identity][fast]") {
  CaptureSink sink;
  ovt::metrics::set_map_sink(&sink);
  Pic r(20, 12, 1, 8);
  ovt::test::fill_synth(r, 0, false);
  Pic d(20, 12, 1, 8);
  d.buf = r.buf;
  d.set(0, 1, 1, r.get(0, 1, 1) + 1U);    // block (0, 0) of 8 x 8: 63 / 64
  d.set(0, 17, 9, r.get(0, 17, 9) + 1U);  // block (2, 1), an edge block of 4 x 4 samples: 15 / 16
  Instance inst(R"({"map_block":8})");
  const Scored s = inst.score(r, d, 3);
  ovt::metrics::set_map_sink(nullptr);
  CHECK(s.map_blob_id == "identity/y/000003/8");
  REQUIRE(sink.maps.size() == 3);
  const auto& y = sink.maps.at("identity/y/000003/8");
  REQUIRE(y.size() == ovt::metrics::kOvtMapHeaderSize + (size_t{3} * 2U * 4U));
  auto at = [&y](size_t i) {
    float f = 0;
    std::memcpy(&f, y.data() + ovt::metrics::kOvtMapHeaderSize + (i * 4), 4);
    return f;
  };
  CHECK(at(0) == 63.0F / 64.0F);
  CHECK(at(1) == 1.0F);
  CHECK(at(2) == 1.0F);
  CHECK(at(3) == 1.0F);
  CHECK(at(4) == 1.0F);
  CHECK(at(5) == 15.0F / 16.0F);
  // The chroma blocks are 4 x 4 samples; U is 10 x 6, so the grid is the luma grid, 3 x 2, the last blocks partial.
  CHECK(sink.maps.at("identity/u/000003/8").size() == y.size());
}

namespace {

// The y4m reader of the ssim tests, reduced to what the bit-flip case needs: 8-bit 4:2:0, frame by frame.
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

TEST_CASE("identity finds the flipped sample of syn_metrics_1080p_bitflip_f37",
          "[metrics][identity][golden][needs-corpus]") {
  const auto flip_path = ovt::test::corpus_or_skip("syn_metrics_1080p_bitflip_f37");
  auto ref_path = ovt::test::corpus_or_skip("syn_metrics_1080p_pair");
  if (ref_path.filename() != "syn_metrics_1080p_ref.y4m") {
    ref_path = ref_path.parent_path() / "syn_metrics_1080p_ref.y4m";  // the entry holds three files
  }
  REQUIRE(std::filesystem::exists(ref_path));
  // tests/corpora/compare_expected.json: {frame, plane, x, y, a, b} (chapter 14 §14.3.3 recipe 52), possibly keyed by
  // the corpus id.
  std::ifstream ef(std::filesystem::path(OVT_CORPORA_COMPARE_EXPECTED));
  REQUIRE(ef);
  auto expected = nlohmann::json::parse(ef);
  if (expected.contains("syn_metrics_1080p_bitflip_f37")) {
    expected = expected.at("syn_metrics_1080p_bitflip_f37");
  }
  const auto frame = expected.at("frame").get<int32_t>();
  Y4m ry(ref_path);
  Y4m dy(flip_path);
  REQUIRE(ry.w_ == dy.w_);
  Pic r(ry.w_, ry.h_, 1, 8);
  Pic d(dy.w_, dy.h_, 1, 8);
  DiagScope diag;
  Instance inst;
  for (int32_t f = 0; f <= frame; ++f) {
    REQUIRE(ry.next(r));
    REQUIRE(dy.next(d));
    const Scored s = inst.score(r, d, f);
    if (f < frame) {
      CHECK(s.values[3] == 1.0);
    } else {
      CHECK(s.values[3] < 1.0);
      CHECK(s.values[0] == (static_cast<double>(r.planes.width[0]) * r.planes.height[0] - 1.0) /
                               (static_cast<double>(r.planes.width[0]) * r.planes.height[0]));
    }
  }
  const Scored fin = inst.finish();
  CHECK(fin.values[2] == 0.0);  // sha256_match
  REQUIRE(diag.diag.entries.size() == 1);
  CHECK(diag.diag.entries[0].code == "VQ-DIFF-0001");
  const std::string want =
      "first differing sample: frame " + std::to_string(frame) + ", plane " +
      std::to_string(expected.at("plane").get<int>()) + ", (" + std::to_string(expected.at("x").get<int>()) + "," +
      std::to_string(expected.at("y").get<int>()) + "): ref " + std::to_string(expected.at("a").get<int>()) + " dist " +
      std::to_string(expected.at("b").get<int>());
  CHECK(diag.diag.entries[0].message == want);
}

// Hidden (run with "[bench]"): single-thread throughput of identity on a 1080p 8-bit 4:2:0 pair, the quantity behind
// FR-VQ-023 (>= 500 fps on the M1 reference); numbers go into docs/acceptance/wi/WI-3.308b.md.
TEST_CASE("identity 1080p single-thread throughput", "[.][bench][metrics][identity]") {
  Pic r(1920, 1080, 1, 8);
  ovt::test::fill_synth(r, 0, false);
  Pic same(1920, 1080, 1, 8);
  same.buf = r.buf;
  Pic differ(1920, 1080, 1, 8);
  differ.buf = r.buf;
  differ.set(0, 1900, 1070, r.get(0, 1900, 1070) + 1U);
  for (const auto& [name, dist] :
       {std::pair<const char*, const Pic*>{"identical", &same}, {"one sample off", &differ}}) {
    Instance inst;
    constexpr int kFrames = 100;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
      (void)inst.score(r, *dist, i);
    }
    const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
    std::printf("identity 1080p, %s: %.0f fps (1 thread)\n", name, kFrames / dt.count());
  }
  SUCCEED();
}
