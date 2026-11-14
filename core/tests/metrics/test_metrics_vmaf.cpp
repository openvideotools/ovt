// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The libvmaf host plugins (chapter 9 §9.5; WI-3.309):
//  - acceptance test 1 (metrics_vmaf_chunking_identity): per-frame vectors on the Netflix pair are identical for
//    one context and for every split into 2..8 overlapping chunks;
//  - acceptance test 2: vmaf_b has 4 values and ci_p95_lo <= bagging <= ci_p95_hi on every frame;
//  - the per-frame values equal the vmaf CLI goldens of WI-0.565 at %.6f;
//  - every generated id's outputs are names libvmaf really writes, the ABI rules and the chunk planner.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "metrics/host/plugin_registry.h"
#include "metrics/vmaf/vmaf_host.h"
#include "ovt_metrics_generated.h"
#include "test_support/corpus.h"

namespace {

namespace vm = ovt::metrics::vmaf;

// A planar picture owning its samples (row padding included so strides differ from the widths).
struct Pic {
  std::array<std::vector<uint8_t>, 3> buf;
  ovt_MetricPlanes planes{};

  Pic(uint32_t w, uint32_t h, uint32_t sub_w, uint32_t sub_h, uint8_t bits, uint8_t nplanes = 3) {
    planes.struct_size = sizeof(planes);
    planes.planes = nplanes;
    planes.bit_depth = bits;
    planes.sample_bytes = bits > 8 ? 2 : 1;
    planes.sample_format = OVT_METRIC_SAMPLE_UINT;
    planes.domain = OVT_METRIC_DOMAIN_CODE_VALUE;
    for (uint8_t p = 0; p < nplanes; ++p) {
      const uint32_t pw = p == 0 ? w : (w + sub_w - 1) / sub_w;
      const uint32_t ph = p == 0 ? h : (h + sub_h - 1) / sub_h;
      const auto stride = static_cast<int32_t>((pw + 7) * planes.sample_bytes);
      buf.at(p).assign(static_cast<size_t>(stride) * ph, 0);
      planes.data[p] = buf.at(p).data();  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      planes.stride_bytes[p] = stride;    // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      planes.width[p] = pw;               // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      planes.height[p] = ph;              // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }
  }
  Pic(const Pic&) = delete;
  Pic& operator=(const Pic&) = delete;
  Pic(Pic&&) = delete;
  Pic& operator=(Pic&&) = delete;
  ~Pic() = default;

  void randomize(std::mt19937& rng) {
    const uint32_t max = (1U << planes.bit_depth) - 1U;
    for (uint8_t p = 0; p < planes.planes; ++p) {
      for (size_t i = 0; i + planes.sample_bytes <= buf.at(p).size(); i += planes.sample_bytes) {
        const uint32_t v = static_cast<uint32_t>(rng()) % (max + 1U);
        buf.at(p)[i] = static_cast<uint8_t>(v & 0xFFU);
        if (planes.sample_bytes == 2) {
          buf.at(p)[i + 1] = static_cast<uint8_t>(v >> 8U);
        }
      }
    }
  }
};

// An 8-bit 4:2:0 Y4M file read whole (the Netflix corpus entries are 576x324, 48 frames, ~13 MB each).
struct Y4m {
  uint32_t w = 0;
  uint32_t h = 0;
  std::vector<std::vector<uint8_t>> frames;

  explicit Y4m(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f);
    std::string header;
    std::getline(f, header);
    REQUIRE(header.rfind("YUV4MPEG2 ", 0) == 0);
    REQUIRE(std::sscanf(header.c_str(), "YUV4MPEG2 W%u H%u", &w, &h) == 2);  // NOLINT(cert-err34-c)
    const size_t size = (static_cast<size_t>(w) * h) + (2 * static_cast<size_t>((w + 1) / 2) * ((h + 1) / 2));
    std::string line;
    while (std::getline(f, line)) {
      REQUIRE(line.rfind("FRAME", 0) == 0);
      std::vector<uint8_t> frame(size);
      f.read(reinterpret_cast<char*>(frame.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
             static_cast<std::streamsize>(size));
      REQUIRE(f.gcount() == static_cast<std::streamsize>(size));
      frames.push_back(std::move(frame));
    }
  }

  [[nodiscard]] ovt_MetricPlanes planes(size_t i) const {
    ovt_MetricPlanes p{};
    p.struct_size = sizeof(p);
    p.planes = 3;
    p.bit_depth = 8;
    p.sample_bytes = 1;
    p.sample_format = OVT_METRIC_SAMPLE_UINT;
    p.domain = OVT_METRIC_DOMAIN_CODE_VALUE;
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    const uint8_t* base = frames.at(i).data();
    const std::array<size_t, 3> offs = {0, static_cast<size_t>(w) * h,
                                        (static_cast<size_t>(w) * h) + (static_cast<size_t>(cw) * ch)};
    for (size_t k = 0; k < 3; ++k) {
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic)
      p.data[k] = base + offs.at(k);
      p.width[k] = k == 0 ? w : cw;
      p.height[k] = k == 0 ? h : ch;
      p.stride_bytes[k] = static_cast<int32_t>(p.width[k]);
      // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    return p;
  }
};

struct Score {
  int32_t frame = -1;
  std::vector<double> values;
};

Score decode(const ovt_Buffer& b) {
  flatbuffers::Verifier v(b.data, b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::FrameScore>(nullptr));
  const auto* fs = flatbuffers::GetRoot<ovt::fb::FrameScore>(b.data);
  Score s;
  REQUIRE(fs->frame() != nullptr);
  s.frame = fs->frame()->decode_index();
  REQUIRE(fs->values() != nullptr);
  s.values.assign(fs->values()->begin(), fs->values()->end());
  return s;
}

// One instance of a libvmaf id.
class Instance {
 public:
  explicit Instance(std::string_view id, const char* options = "{}") : api_(vm::metric_api(id)) {
    REQUIRE(api_ != nullptr);
    REQUIRE(api_->create(options, &m_) == OVT_OK);
  }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;
  ~Instance() { api_->destroy(m_); }

  // Scores a frame; appends the FrameScore delivered by this call, if any.
  ovt_status score(const ovt_MetricPlanes& r, const ovt_MetricPlanes& d, int32_t frame, std::vector<Score>& out) {
    ovt_Buffer b{};
    const ovt_status st = api_->score(m_, &r, &d, ovt_FrameId{frame, frame}, &b);
    if (st == OVT_OK && b.data != nullptr) {
      out.push_back(decode(b));
    }
    return st;
  }
  ovt_status finish(std::vector<Score>& out) {
    ovt_Buffer b{};
    const ovt_status st = api_->finish(m_, &b);
    if (st == OVT_OK && b.data != nullptr) {
      out.push_back(decode(b));
    }
    return st;
  }

 private:
  const ovt_MetricApi* api_;
  ovt_Metric* m_ = nullptr;
};

// Per-frame values of `id` over frames [0, n) of the pair, split into `chunks` overlapping chunks.
std::vector<std::vector<double>> run_chunked(std::string_view id, const Y4m& ref, const Y4m& dist, uint32_t chunks,
                                             uint32_t lookbehind = vm::kLookbehind,
                                             uint32_t lookahead = vm::kLookahead) {
  const auto n = static_cast<uint32_t>(ref.frames.size());
  std::vector<std::vector<double>> result(n);
  for (const auto& c : vm::plan_chunks(n, chunks, lookbehind, lookahead)) {
    Instance inst(id);
    std::vector<Score> got;
    for (uint32_t f = c.feed_begin; f < c.feed_end; ++f) {
      const auto r = ref.planes(f);
      const auto d = dist.planes(f);
      REQUIRE(inst.score(r, d, static_cast<int32_t>(f), got) == OVT_OK);
    }
    REQUIRE(inst.finish(got) == OVT_OK);
    REQUIRE(got.size() == c.feed_end - c.feed_begin);
    for (const auto& s : got) {
      const auto f = static_cast<uint32_t>(s.frame);
      if (f >= c.keep_begin && f < c.keep_end) {
        REQUIRE(result.at(f).empty());
        result.at(f) = s.values;
      }
    }
  }
  for (const auto& v : result) {
    REQUIRE(!v.empty());
  }
  return result;
}

// The text of a .gz file (gzip is on every desktop CI image; the tests that use it need the corpus anyway).
std::string gunzip(const std::filesystem::path& path) {
  const std::string cmd = "gzip -dc '" + path.string() + "'";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"),  // NOLINT(cert-env33-c): fixed command
                                             pclose);
  REQUIRE(pipe != nullptr);
  std::string text;
  std::array<char, 65536> buf{};
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), pipe.get())) > 0) {
    text.append(buf.data(), n);
  }
  REQUIRE(!text.empty());
  return text;
}

std::string fmt6(double v) {
  std::array<char, 64> b{};
  std::snprintf(b.data(), b.size(), "%.6f", v);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  return b.data();
}

}  // namespace

TEST_CASE("vmaf plan_chunks splits the range and adds one overlap frame per side", "[metrics][vmaf][fast]") {
  CHECK(vm::plan_chunks(0, 4).empty());
  CHECK(vm::plan_chunks(10, 1) == std::vector<vm::Chunk>{{0, 0, 10, 10}});
  CHECK(vm::plan_chunks(10, 0) == std::vector<vm::Chunk>{{0, 0, 10, 10}});
  CHECK(vm::plan_chunks(10, 3) == std::vector<vm::Chunk>{{0, 0, 4, 5}, {3, 4, 7, 8}, {6, 7, 10, 10}});
  CHECK(vm::plan_chunks(3, 8) == std::vector<vm::Chunk>{{0, 0, 1, 2}, {0, 1, 2, 3}, {1, 2, 3, 3}});
  CHECK(vm::plan_chunks(10, 2, 2, 0) == std::vector<vm::Chunk>{{0, 0, 5, 5}, {3, 5, 10, 10}});
}

TEST_CASE("vmaf models: the generated table holds the named ids and valid descriptors", "[metrics][vmaf][fast]") {
  std::set<std::string> ids;
  for (const auto& m : vm::models()) {
    ids.insert(m.id);
    CHECK(m.output_count() >= 4);
    const auto* api = vm::metric_api(m.id);
    REQUIRE(api != nullptr);
    if (!vm::model_available(m)) {
      CHECK(m.needs_float);  // only the float models may be missing (libvmaf built without enable_float)
      continue;
    }
    const std::string id = m.id;
    std::string error;
    const auto bytes = ovt::metrics::validate_metric_api(api, &id, error);
    INFO(m.id << ": " << error);
    REQUIRE(bytes.has_value());
    const auto* d = flatbuffers::GetRoot<ovt::fb::MetricDescriptor>(bytes->data());
    CHECK(d->kind() == ovt::fb::MetricKind::FullReference);
    CHECK(d->domain() == ovt::fb::WorkingDomain::CodeValue);
    CHECK(d->temporal_unit_frames() == 1);
    REQUIRE(d->outputs() != nullptr);
    CHECK(d->outputs()->size() == m.output_count());
    CHECK(d->license()->str() == "BSD-2-Clause-Patent");
  }
  for (const char* id : {"vmaf", "vmaf_neg", "vmaf_4k", "vmaf_b"}) {
    CHECK(ids.count(id) == 1);
    CHECK(vm::model_available(*std::find_if(vm::models().begin(), vm::models().end(),
                                            [&](const vm::ModelInfo& m) { return std::string_view(m.id) == id; })));
  }
  CHECK(vm::metric_api("nosuch") == nullptr);
  CHECK(vm::engine().rfind("libvmaf ", 0) == 0);
}

TEST_CASE("vmaf registry: the available libvmaf ids are registered as built-ins", "[metrics][vmaf][fast]") {
  ovt::metrics::PluginRegistry::Options o;
  o.ovt_metric_path = "";
  o.ovt_plugin_path = "";
  o.defaults = std::vector<std::string>{};
  ovt::metrics::PluginRegistry registry(o);
  const size_t n = vm::register_builtin_vmaf(registry);
  CHECK(n >= 4);
  for (const char* id : {"vmaf", "vmaf_neg", "vmaf_4k", "vmaf_b"}) {
    const auto* p = registry.find(id);
    REQUIRE(p != nullptr);
    CHECK(p->origin == "(built-in)");
  }
  CHECK(ovt::metrics::PluginRegistry::instance().find("vmaf") != nullptr);
}

TEST_CASE("vmaf every available id scores frames and resolves each output name", "[metrics][vmaf][fast]") {
  std::mt19937 rng(7);  // NOLINT(cert-msc32-c,cert-msc51-cpp): a fixed seed keeps the case reproducible
  for (const auto& m : vm::models()) {
    if (!vm::model_available(m)) {
      continue;
    }
    INFO(m.id);
    Instance inst(m.id);
    std::vector<Score> got;
    for (int32_t f = 0; f < 4; ++f) {
      Pic r(176, 144, 2, 2, 8);
      Pic d(176, 144, 2, 2, 8);
      r.randomize(rng);
      d.randomize(rng);
      REQUIRE(inst.score(r.planes, d.planes, f * 2, got) == OVT_OK);
      // The score of frame t arrives with frame t+1 (integer_motion2 needs it).
      REQUIRE(got.size() == static_cast<size_t>(f));
    }
    REQUIRE(inst.finish(got) == OVT_OK);
    REQUIRE(got.size() == 4);
    for (size_t i = 0; i < got.size(); ++i) {
      CHECK(got[i].frame == static_cast<int32_t>(i) * 2);
      CHECK(got[i].values.size() == m.output_count());
      for (const double v : got[i].values) {
        CHECK(std::isfinite(v));
      }
    }
    CHECK(inst.finish(got) == OVT_E_INVALID_ARG);  // finish() ends the instance
  }
}

TEST_CASE("vmaf accepts 4:0:0, 4:2:2, 4:4:4 and 10/12/16-bit input", "[metrics][vmaf][fast]") {
  std::mt19937 rng(11);  // NOLINT(cert-msc32-c,cert-msc51-cpp)
  struct Case {
    uint32_t sw, sh;
    uint8_t bits, planes;
  };
  for (const Case c : {Case{1, 1, 8, 1}, Case{2, 1, 8, 3}, Case{1, 1, 8, 3}, Case{2, 2, 10, 3}, Case{2, 2, 12, 3},
                       Case{2, 2, 16, 3}}) {
    INFO(c.sw << "x" << c.sh << " " << int{c.bits} << " bit, " << int{c.planes} << " planes");
    Instance inst("vmaf");
    std::vector<Score> got;
    for (int32_t f = 0; f < 3; ++f) {
      Pic r(97, 61, c.sw, c.sh, c.bits, c.planes);  // odd sizes: chroma is (w + 1) / 2
      Pic d(97, 61, c.sw, c.sh, c.bits, c.planes);
      r.randomize(rng);
      d.randomize(rng);
      REQUIRE(inst.score(r.planes, d.planes, f, got) == OVT_OK);
    }
    REQUIRE(inst.finish(got) == OVT_OK);
    CHECK(got.size() == 3);
  }
}

TEST_CASE("vmaf rejects bad options and bad frames", "[metrics][vmaf][fast]") {
  const auto* api = vm::metric_api("vmaf");
  const auto* api_b = vm::metric_api("vmaf_b");
  REQUIRE(api != nullptr);
  REQUIRE(api_b != nullptr);
  ovt_Metric* m = nullptr;
  CHECK(api->create(R"({"nosuch":1})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(R"({"phone":1})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create("[]", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create("{", &m) == OVT_E_INVALID_ARG);
  CHECK(api_b->create(R"({"phone":true})", &m) == OVT_E_INVALID_ARG);
  CHECK(api->create(nullptr, &m) == OVT_E_INVALID_ARG);
  CHECK(m == nullptr);

  std::mt19937 rng(3);  // NOLINT(cert-msc32-c,cert-msc51-cpp)
  Pic r(64, 64, 2, 2, 8);
  Pic d(64, 64, 2, 2, 8);
  r.randomize(rng);
  d.randomize(rng);
  std::vector<Score> got;
  {
    Instance inst("vmaf", R"({"phone":true})");
    Pic d10(64, 64, 2, 2, 10);
    CHECK(inst.score(r.planes, d10.planes, 0, got) == OVT_E_INVALID_ARG);  // depths differ
    Pic fl(64, 64, 2, 2, 8);
    fl.planes.domain = OVT_METRIC_DOMAIN_LINEAR_LIGHT;
    CHECK(inst.score(r.planes, fl.planes, 0, got) == OVT_E_INVALID_ARG);  // not CodeValue
    Pic small(32, 64, 2, 2, 8);
    CHECK(inst.score(r.planes, small.planes, 0, got) == OVT_E_INVALID_ARG);  // geometry differs
    Pic b9(64, 64, 2, 2, 9);
    CHECK(inst.score(b9.planes, b9.planes, 0, got) == OVT_E_INVALID_ARG);  // libvmaf takes 8/10/12/16 bits
    REQUIRE(inst.score(r.planes, d.planes, 5, got) == OVT_OK);
    CHECK(inst.score(r.planes, d.planes, 5, got) == OVT_E_INVALID_ARG);  // decode_index must increase (rule 2)
    Pic big(128, 64, 2, 2, 8);
    CHECK(inst.score(big.planes, big.planes, 6, got) == OVT_E_INVALID_ARG);  // geometry fixed by frame 0
    REQUIRE(inst.score(r.planes, d.planes, 6, got) == OVT_OK);
    REQUIRE(inst.finish(got) == OVT_OK);
    CHECK(inst.score(r.planes, d.planes, 7, got) == OVT_E_INVALID_ARG);  // after finish()
    REQUIRE(got.size() == 2);
    CHECK(got[0].frame == 5);
    CHECK(got[1].frame == 6);
  }
  {
    Instance inst("vmaf");
    std::vector<Score> none;
    REQUIRE(inst.finish(none) == OVT_OK);  // no frame: nothing to deliver
    CHECK(none.empty());
  }
}

TEST_CASE("metrics_vmaf_chunking_identity", "[metrics][vmaf][golden][needs-corpus]") {
  // Acceptance test 1. A 48-frame pair gives one chunk at any thread count (§9.2.3: ceil(48 / 240) = 1), so the
  // chunk split is forced here: every split into 1..8 chunks must give the same per-frame vectors, bit for bit.
  const Y4m ref(ovt::test::corpus_or_skip("netflix_src01_hrc00_576x324"));
  const Y4m dist(ovt::test::corpus_or_skip("netflix_src01_hrc01_576x324"));
  REQUIRE(ref.frames.size() == 48);
  REQUIRE(dist.frames.size() == 48);
  for (const char* id : {"vmaf", "vmaf_b"}) {
    INFO(id);
    const auto single = run_chunked(id, ref, dist, 1);
    for (const uint32_t chunks : {2U, 3U, 5U, 8U}) {
      INFO(chunks << " chunks");
      CHECK(run_chunked(id, ref, dist, chunks) == single);
    }
  }
  // The overlap is needed on both sides (#394): without the lookahead frame, integer_motion2 of a chunk's last
  // frame is libvmaf's end-of-sequence value motion(e) instead of min(motion(e), motion(e + 1)), so some split
  // differs from the single run, and only on the last frame of a chunk.
  const auto single = run_chunked("vmaf", ref, dist, 1);
  bool any_differs = false;
  for (const uint32_t chunks : {2U, 3U, 5U, 8U}) {
    const auto plan = vm::plan_chunks(48, chunks);
    const auto no_lookahead = run_chunked("vmaf", ref, dist, chunks, 1, 0);
    for (size_t f = 0; f < single.size(); ++f) {
      if (no_lookahead[f] != single[f]) {
        any_differs = true;
        CHECK(std::any_of(plan.begin(), plan.end(), [&](const vm::Chunk& c) { return c.keep_end == f + 1; }));
      }
    }
  }
  CHECK(any_differs);
}

// The WI-0.565 goldens come from the x86_64 oracle image. libvmaf's float features differ by ~1e-5 between x86_64
// builds and by up to ~3e-3 VMAF on arm64 (NEON paths, FMA contraction); even the integer models differ in the last
// %.6f digit on arm64 (max seen 8e-6). Bit-exactness on arm64 is checked against a CLI built on the same machine
// (WI-3.591, WI-3.317), not here.
#if defined(__x86_64__) || defined(_M_X64)
constexpr bool kGoldenArch = true;
#else
constexpr bool kGoldenArch = false;
#endif
constexpr double kIntegerTolerance = 5e-5;
constexpr double kFloatTolerance = kGoldenArch ? 1e-4 : 1e-2;

TEST_CASE("vmaf per-frame values equal the vmaf CLI on the Netflix pair", "[metrics][vmaf][golden][needs-corpus]") {
  // The vmaf CLI goldens of WI-0.565 (tests/golden/metrics/<pair>.<model>.vmaf.json.gz, same libvmaf tag and Meson
  // options): every output of every model id that has a golden, over a 3-chunk run; at the CLI's %.6f for the
  // integer models on x86_64, within the tolerances above otherwise.
  const Y4m ref(ovt::test::corpus_or_skip("netflix_src01_hrc00_576x324"));
  const Y4m dist(ovt::test::corpus_or_skip("netflix_src01_hrc01_576x324"));
  size_t compared = 0;
  for (const auto& m : vm::models()) {
    const auto path = std::filesystem::path(OVT_GOLDEN_METRICS_DIR) /
                      (std::string("netflix_src01_576x324.") + m.version + ".vmaf.json.gz");
    if (!std::filesystem::exists(path) || !vm::model_available(m)) {
      continue;
    }
    INFO(m.id);
    const auto golden = nlohmann::json::parse(gunzip(path));
    const auto& frames = golden.at("frames");
    const auto got = run_chunked(m.id, ref, dist, 3);
    REQUIRE(frames.size() == got.size());
    for (size_t f = 0; f < got.size(); ++f) {
      REQUIRE(got[f].size() == m.output_count());
      for (size_t k = 0; k < got[f].size(); ++k) {
        // A collection's outputs are the CLI keys vmaf_bagging, vmaf_ci_p95_lo, ... (model score name "vmaf").
        const std::string key = m.collection ? std::string("vmaf_") + m.outputs.at(k) : std::string(m.outputs.at(k));
        INFO("frame " << f << " " << key);
        const double want = frames[f].at(key).get<double>();
        if (kGoldenArch && !m.needs_float) {
          CHECK(fmt6(got[f][k]) == fmt6(want));  // the integer path, on the goldens' architecture: bit-exact
        } else {
          CHECK(std::abs(got[f][k] - want) <= (m.needs_float ? kFloatTolerance : kIntegerTolerance));
        }
      }
    }
    ++compared;
  }
  CHECK(compared >= 4);  // vmaf, vmaf_neg, vmaf_4k, vmaf_b at least
}

TEST_CASE("vmaf_b has four values with the bagging score inside its 95% interval",
          "[metrics][vmaf][golden][needs-corpus]") {
  // Acceptance test 2.
  const Y4m ref(ovt::test::corpus_or_skip("netflix_src01_hrc00_576x324"));
  const Y4m dist(ovt::test::corpus_or_skip("netflix_src01_hrc01_576x324"));
  const auto got = run_chunked("vmaf_b", ref, dist, 1);
  REQUIRE(got.size() == 48);
  for (const auto& v : got) {
    REQUIRE(v.size() == 4);
    CHECK(v[1] <= v[0]);
    CHECK(v[0] <= v[2]);
    CHECK(v[3] >= 0.0);
  }
}
