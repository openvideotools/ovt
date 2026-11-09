// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Smoke and acceptance tests of ovt-plugin-aom (WI-2.155, chapter 07 §7.6.1), loaded through the host's
// PluginRegistry (WI-2.150) and driven through its ABI only. The libaom test vectors are read from IVF and pushed
// one OBU per call with the decode_index_hint the host index gives each frame's units (§7.5.3 A4): a frame OBU
// starts a new frame; the temporal delimiter and the other OBUs in front of a frame travel with that frame.
// AT1: one FrameAnalysis per frame with tile data (hidden included), with the matching decode_index.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "aom_test_util.inc"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

using ovt::aom::test::Instance;
using ovt::aom::test::Obu;
using ovt::aom::test::plugin;
using ovt::aom::test::read_ivf;
using ovt::aom::test::TemporalUnit;

namespace {

constexpr uint32_t kReconMask = 1U << 0;  // ovt.fb.PlaneStage.Reconstruction

// ---- driving the plugin like the host -----------------------------------------------------------------------------

struct Result {
  int32_t decode_index = -1;
  bool shown = false;
  int32_t coded_w = 0;
  int32_t coded_h = 0;
  std::vector<uint32_t> plane_w;  // per PlaneRef
  std::vector<uint32_t> plane_h;
  std::vector<size_t> blob_size;  // bytes returned by plane()
};

struct Run {
  int32_t frames = 0;             // frame OBUs = FrameInfo entries of the host index
  std::vector<int32_t> expected;  // decode indices of frames with tile data (not show_existing)
  std::vector<Result> results;
};

void drain(Instance& inst, uint32_t mask, Run* run) {
  for (;;) {
    ovt_Buffer fb{};
    REQUIRE(inst.api.pull(inst.dec, mask, &fb) == OVT_OK);
    if (fb.data == nullptr) {
      return;
    }
    flatbuffers::Verifier verifier(fb.data, fb.size);
    REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
    const ovt::fb::FrameAnalysis* fa = ovt::fb::GetFrameAnalysis(fb.data);
    REQUIRE(fa->frame() != nullptr);
    REQUIRE(fa->plugin_id()->str() == "aom");
    Result r;
    r.decode_index = fa->frame()->id()->decode_index();
    r.shown = fa->frame()->shown();
    r.coded_w = fa->frame()->coded_size()->w();
    r.coded_h = fa->frame()->coded_size()->h();
    for (const ovt::fb::PlaneRef* p : *fa->planes()) {
      r.plane_w.push_back(p->width());
      r.plane_h.push_back(p->height());
      const uint8_t* data = nullptr;
      size_t size = 0;
      REQUIRE(inst.api.plane(inst.dec, p->blob_id()->c_str(), &data, &size) == OVT_OK);
      r.blob_size.push_back(size);
    }
    inst.api.release(inst.dec, fb);
    run->results.push_back(std::move(r));
  }
}

// Pushes every OBU of @p tus; @p hint_skew is added to the hints from temporal unit @p skew_from on.
Run analyse(const std::vector<TemporalUnit>& tus, uint32_t mask, int32_t skew_from = -1, int32_t hint_skew = 0) {
  Instance inst;
  REQUIRE(inst.status == OVT_OK);
  Run run;
  int32_t next = 0;
  for (size_t t = 0; t < tus.size(); ++t) {
    const int32_t skew = skew_from >= 0 && static_cast<int32_t>(t) >= skew_from ? hint_skew : 0;
    int32_t cur = -1;
    for (const Obu& o : tus[t]) {
      const bool frame_obu = o.type == 3 || o.type == 6;
      if (frame_obu) {
        cur = next++;
        if (!o.show_existing) {
          run.expected.push_back(cur);
        }
      }
      const int32_t hint = (cur >= 0 ? cur : next) + skew;
      const ovt_status st = inst.api.push(inst.dec, o.bytes.data(), o.bytes.size(), hint, hint);
      if (st == OVT_E_PLUGIN) {
        run.frames = -1;  // the frame-count check fired
        return run;
      }
      REQUIRE(st == OVT_OK);
      drain(inst, mask, &run);
    }
  }
  REQUIRE(inst.api.flush(inst.dec) == OVT_OK);
  drain(inst, mask, &run);
  run.frames = next;
  return run;
}

std::vector<int32_t> indices(const Run& run) {
  std::vector<int32_t> out;
  out.reserve(run.results.size());
  for (const Result& r : run.results) {
    out.push_back(r.decode_index);
  }
  return out;
}

}  // namespace

TEST_CASE("aom plugin: caps through the registry", "[fast][third_party]") {
  const ovt_AnalysisCaps* caps = plugin().api->caps();
  REQUIRE(caps != nullptr);
  CHECK(caps->codec == 3);  // ovt.fb.Codec.AV1
  CHECK(caps->plane_stages == kReconMask);
  // §7.6.1 Caps row with stock libaom and no accounting yet: all but tx_type, bits_x8 and deblock_bs.
  CHECK(caps->grid_fields == 0U);
  CHECK(std::string(caps->id) == "aom");
  CHECK(std::string(caps->version).find("libaom") != std::string::npos);
  const Instance inst;
  CHECK(inst.status == OVT_OK);
}

TEST_CASE("aom plugin AT1: one FrameAnalysis per frame of av1-1-b8-01-size-66x66",
          "[fast][third_party][needs-corpus]") {
  const auto tus = read_ivf(ovt::test::corpus_or_skip("aom_av1_1_b8_01_size_66x66"));
  const Run run = analyse(tus, kReconMask);
  REQUIRE(run.frames > 0);
  CHECK(indices(run) == run.expected);
  for (const Result& r : run.results) {
    CHECK(r.coded_w == 66);
    CHECK(r.coded_h == 66);
    if (!r.shown) {
      CHECK(r.plane_w.empty());
      continue;
    }
    REQUIRE(r.plane_w.size() == 3);
    CHECK(r.plane_w == std::vector<uint32_t>{66, 33, 33});
    CHECK(r.plane_h == std::vector<uint32_t>{66, 33, 33});
    CHECK(r.blob_size == std::vector<size_t>{4356, 1089, 1089});  // 66 x 66 and 33 x 33 8-bit samples
  }
}

TEST_CASE("aom plugin: hidden frames and show_existing in av1-1-b8-05-mv", "[fast][third_party][needs-corpus]") {
  const auto tus = read_ivf(ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv"));
  const Run run = analyse(tus, kReconMask);
  CHECK(indices(run) == run.expected);
  CHECK(run.expected.size() < static_cast<size_t>(run.frames));  // the stream has show_existing frames
  size_t hidden = 0;
  for (const Result& r : run.results) {
    hidden += r.shown ? 0 : 1;
    CHECK(r.plane_w.size() == (r.shown ? 3U : 0U));  // hidden frames have no output image
  }
  CHECK(hidden > 0);
}

TEST_CASE("aom plugin: no planes when Reconstruction is not requested", "[fast][third_party][needs-corpus]") {
  const auto tus = read_ivf(ovt::test::corpus_or_skip("aom_av1_1_b8_01_size_66x66"));
  const Run run = analyse(tus, 0);
  REQUIRE(!run.results.empty());
  for (const Result& r : run.results) {
    CHECK(r.plane_w.empty());
  }
}

TEST_CASE("aom plugin: hints that disagree with libaom's frame count", "[fast][third_party][needs-corpus]") {
  const auto tus = read_ivf(ovt::test::corpus_or_skip("aom_av1_1_b8_01_size_66x66"));
  REQUIRE(tus.size() > 1);
  const Run run = analyse(tus, 0, /*skew_from=*/1, /*hint_skew=*/1);
  CHECK(run.frames == -1);
}

TEST_CASE("aom plugin: malformed OBUs return OVT_E_CORRUPT", "[fast][third_party]") {
  const Instance inst;
  REQUIRE(inst.status == OVT_OK);
  const std::vector<uint8_t> no_size = {0x30, 0x00};         // OBU_FRAME without obu_has_size_field
  const std::vector<uint8_t> bad_size = {0x32, 0x05, 0x00};  // obu_size 5, one payload byte
  const std::vector<uint8_t> forbidden = {0x92, 0x00};       // obu_forbidden_bit
  CHECK(inst.api.push(inst.dec, no_size.data(), no_size.size(), 0, 0) == OVT_E_CORRUPT);
  CHECK(inst.api.push(inst.dec, bad_size.data(), bad_size.size(), 0, 0) == OVT_E_CORRUPT);
  CHECK(inst.api.push(inst.dec, forbidden.data(), forbidden.size(), 0, 0) == OVT_E_CORRUPT);
  // A well-formed frame OBU without a sequence header: libaom rejects it at flush, nothing crashes.
  const std::vector<uint8_t> td = {0x12, 0x00};
  const std::vector<uint8_t> frame = {0x32, 0x03, 0xFF, 0xFF, 0xFF};
  CHECK(inst.api.push(inst.dec, td.data(), td.size(), 0, 0) == OVT_OK);
  CHECK(inst.api.push(inst.dec, frame.data(), frame.size(), 0, 0) == OVT_OK);
  CHECK(inst.api.flush(inst.dec) == OVT_E_CORRUPT);
  ovt_Buffer fb{};
  CHECK(inst.api.pull(inst.dec, kReconMask, &fb) == OVT_OK);
  CHECK(fb.data == nullptr);
}
