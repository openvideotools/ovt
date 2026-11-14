// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Plane snapshots of ovt-plugin-de265: libde265 patch 0003 and its PlaneRef export (WI-2.166, chapter 07 §7.6.2).
// The plugin is loaded through ovt::analysis::PluginRegistry like the analysis host loads it; the reference decodes
// use the same patched libde265 directly (linked into this test), with its in-loop filters switched off.
//  - AT1 `de265_predeblock_vs_filters_off`: on the intra-only ipcm_A_NEC_3, every PreDeblock plane equals a decode
//    with DE265_DECODER_PARAM_DISABLE_DEBLOCKING and DISABLE_SAO.
//  - AT2 `de265_pred_plus_resid`: on every frame of five streams (intra, inter, 10-bit, WPP with threads,
//    transform skip), Prediction + Residual equals PreDeblock for every sample.
//  - AT3 `de265_snapshots_off_memory`: with snapshots off (no pull asks for a patch-0003 stage) libde265 allocates
//    no snapshot memory and the plugin exports and keeps no snapshot plane.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <flatbuffers/flatbuffers.h>
#include <libde265/de265.h>
#include <libde265/de265_internals.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

// ovt.fb.PlaneStage bits.
constexpr uint32_t kRecon = 1U << 0;
constexpr uint32_t kPrediction = 1U << 1;
constexpr uint32_t kResidual = 1U << 2;
constexpr uint32_t kPreDeblock = 1U << 3;
constexpr uint32_t kPreSao = 1U << 4;
constexpr uint32_t kCoefficients = 1U << 6;
constexpr uint32_t kSnapshotStages = kPrediction | kResidual | kPreDeblock | kPreSao | kCoefficients;
constexpr uint32_t kAllStages = kRecon | kSnapshotStages;

const ovt_AnalysisDecoderApi& plugin_api() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_DE265_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("de265");
  REQUIRE(plugin != nullptr);
  return *plugin->api;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Nal {
  const uint8_t* data;
  size_t size;
};

std::vector<Nal> split_annexb(const std::vector<uint8_t>& s) {
  std::vector<Nal> nals;
  size_t i = 0;
  size_t start = SIZE_MAX;
  while (i + 3 <= s.size()) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != SIZE_MAX) {
        size_t end = i;
        while (end > start && s[end - 1] == 0) {
          --end;
        }
        nals.push_back({s.data() + start, end - start});
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX && start < s.size()) {
    nals.push_back({s.data() + start, s.size() - start});
  }
  return nals;
}

// One exported plane, copied out of the plugin before release().
struct PlaneCopy {
  std::vector<uint8_t> data;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  uint8_t bit_depth = 0;
  uint8_t sample_bytes = 0;
  bool is_signed = false;

  [[nodiscard]] int64_t at(uint32_t x, uint32_t y) const {
    const std::size_t off = (std::size_t{y} * stride_bytes) + (std::size_t{x} * sample_bytes);
    switch (sample_bytes) {
      case 1:
        return data[off];
      case 2: {
        uint16_t u = 0;
        std::memcpy(&u, &data[off], sizeof u);
        return is_signed ? int64_t{static_cast<int16_t>(u)} : int64_t{u};
      }
      default: {
        int32_t v = 0;
        std::memcpy(&v, &data[off], sizeof v);
        return v;
      }
    }
  }
};

struct FrameOut {
  int decode_index = -1;
  ovt::fb::FrameType type = ovt::fb::FrameType::Unknown;
  int coded_w = 0;
  int coded_h = 0;
  int crop_x = 0;
  int crop_y = 0;
  int crop_w = 0;
  int crop_h = 0;
  std::map<int, std::array<PlaneCopy, 3>> stages;  // PlaneStage value -> Y, U, V
};

struct Instance {
  explicit Instance(const ovt_AnalysisDecoderApi& a, int threads) : api(a) {
    status = api.create(nullptr, 0, threads, &dec);
  }
  ~Instance() {
    if (dec != nullptr) {
      api.destroy(dec);
    }
  }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;
  const ovt_AnalysisDecoderApi& api;
  ovt_AnalysisDecoder* dec = nullptr;
  ovt_status status = OVT_E_INTERNAL;
};

void drain(Instance& in, uint32_t mask, std::vector<FrameOut>* out) {
  for (;;) {
    ovt_Buffer fb{};
    REQUIRE(in.api.pull(in.dec, mask, &fb) == OVT_OK);
    if (fb.data == nullptr) {
      return;
    }
    flatbuffers::Verifier verifier(fb.data, fb.size);
    REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
    const ovt::fb::FrameAnalysis* fa = ovt::fb::GetFrameAnalysis(fb.data);
    REQUIRE(fa->frame() != nullptr);
    FrameOut f;
    f.decode_index = fa->frame()->id()->decode_index();
    f.type = fa->frame()->type();
    REQUIRE(fa->frame()->coded_size() != nullptr);
    REQUIRE(fa->frame()->crop() != nullptr);
    f.coded_w = fa->frame()->coded_size()->w();
    f.coded_h = fa->frame()->coded_size()->h();
    f.crop_x = fa->frame()->crop()->x();
    f.crop_y = fa->frame()->crop()->y();
    f.crop_w = fa->frame()->crop()->w();
    f.crop_h = fa->frame()->crop()->h();
    REQUIRE(fa->planes() != nullptr);
    for (const ovt::fb::PlaneRef* p : *fa->planes()) {
      const auto stage = static_cast<int>(p->stage());
      CHECK((mask & (1U << static_cast<unsigned>(stage))) != 0);  // only requested stages are exported
      // Chapter 06 §6.6.2 local blob id and the V-023 shape rules.
      static constexpr std::array<std::string_view, 7> kTokens = {"recon",      "pred",  "resid", "predeblock",
                                                                  "presaocdef", "prelr", "coeff"};
      static constexpr std::array<std::string_view, 3> kPlanes = {"y", "u", "v"};
      CHECK(p->blob_id()->str() == std::to_string(f.decode_index) + "/" +
                                       std::string(kTokens.at(static_cast<std::size_t>(stage))) + "/" +
                                       std::string(kPlanes.at(p->plane())));
      CHECK(p->stride_bytes() >= p->width() * p->sample_bytes());
      CHECK((p->sample_bytes() == 1) == (p->bit_depth() == 8 && !p->is_signed()));
      if (p->stage() == ovt::fb::PlaneStage::Residual) {
        CHECK((p->is_signed() && p->sample_bytes() == 2));
      } else if (p->stage() == ovt::fb::PlaneStage::Coefficients) {
        CHECK((p->is_signed() && p->sample_bytes() == 4));
      } else {
        CHECK(!p->is_signed());
      }
      if (p->plane() == 0) {
        CHECK(static_cast<int>(p->width()) == f.coded_w);
        CHECK(static_cast<int>(p->height()) == f.coded_h);
      }
      const uint8_t* data = nullptr;
      size_t size = 0;
      REQUIRE(in.api.plane(in.dec, p->blob_id()->c_str(), &data, &size) == OVT_OK);
      REQUIRE(size ==
              (std::size_t{p->stride_bytes()} * (p->height() - 1)) + (std::size_t{p->width()} * p->sample_bytes()));
      PlaneCopy c{std::vector<uint8_t>(data, data + size),
                  p->width(),
                  p->height(),
                  p->stride_bytes(),
                  p->bit_depth(),
                  p->sample_bytes(),
                  p->is_signed()};
      f.stages[stage].at(p->plane()) = std::move(c);
    }
    out->push_back(std::move(f));
    in.api.release(in.dec, fb);
  }
}

// Decodes @p bytes through the plugin, pulling with @p mask after every unit. @p prime pulls once before the first
// unit, so that the snapshots cover the first picture already (a pull asking for a patch-0003 stage turns them on
// for the pictures that start decoding afterwards). Without it, the first @p units_before_pull units are pushed
// before anything is pulled.
std::vector<FrameOut> run_plugin(const std::vector<uint8_t>& bytes, int threads, uint32_t mask, bool prime = true,
                                 std::size_t units_before_pull = 0) {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api, threads);
  REQUIRE(in.status == OVT_OK);
  std::vector<FrameOut> frames;
  if (prime) {
    drain(in, mask, &frames);
    REQUIRE(frames.empty());
  }
  int pic = -1;
  std::size_t pushed = 0;
  for (const Nal& n : split_annexb(bytes)) {
    if (n.size < 3) {
      continue;
    }
    const int type = (n.data[0] >> 1) & 0x3F;
    if (type < 32 && (n.data[2] & 0x80) != 0) {
      ++pic;
    }
    REQUIRE(api.push(in.dec, n.data, n.size, std::max(pic, 0), std::max(pic, 0)) == OVT_OK);
    if (++pushed > units_before_pull) {
      drain(in, mask, &frames);
    }
  }
  REQUIRE(api.flush(in.dec) == OVT_OK);
  drain(in, mask, &frames);
  return frames;
}

// The cropped output pictures of a direct libde265 decode, in output order (as de265_get_image_plane gives them).
struct RefPicture {
  std::array<std::vector<uint8_t>, 3> planes;
  std::array<int, 3> width{};
  std::array<int, 3> height{};
  std::array<int, 3> bytes{};
};

std::vector<RefPicture> run_reference(const std::vector<uint8_t>& bytes, bool deblocking, bool sao,
                                      bool snapshots = false, uint64_t* peak = nullptr) {
  de265_decoder_context* ctx = de265_new_decoder();
  REQUIRE(ctx != nullptr);
  de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_DISABLE_DEBLOCKING, deblocking ? 0 : 1);
  de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_DISABLE_SAO, sao ? 0 : 1);
  de265_internals_set_plane_snapshots(ctx, snapshots ? 1 : 0);
  REQUIRE(de265_isOK(de265_push_data(ctx, bytes.data(), static_cast<int>(bytes.size()), 0, nullptr)) != 0);
  REQUIRE(de265_isOK(de265_flush_data(ctx)) != 0);
  std::vector<RefPicture> out;
  int more = 1;
  for (int calls = 0; more != 0 && calls < 1000000; ++calls) {
    more = 0;
    const de265_error err = de265_decode(ctx, &more);
    REQUIRE((de265_isOK(err) != 0 || err == DE265_ERROR_WAITING_FOR_INPUT_DATA));
    while (const de265_image* img = de265_peek_next_picture(ctx)) {
      RefPicture p;
      const int planes = de265_get_chroma_format(img) == de265_chroma_mono ? 1 : 3;
      for (int c = 0; c < planes; ++c) {
        const auto ci = static_cast<std::size_t>(c);
        int stride = 0;
        const uint8_t* d = de265_get_image_plane(img, c, &stride);
        p.width.at(ci) = de265_get_image_width(img, c);
        p.height.at(ci) = de265_get_image_height(img, c);
        p.bytes.at(ci) = (de265_get_bits_per_pixel(img, c) + 7) / 8;
        const std::size_t row = static_cast<std::size_t>(p.width.at(ci)) * static_cast<std::size_t>(p.bytes.at(ci));
        for (int y = 0; y < p.height.at(ci); ++y) {
          const uint8_t* line = d + (static_cast<std::size_t>(y) * static_cast<std::size_t>(stride));
          p.planes.at(ci).insert(p.planes.at(ci).end(), line, line + row);
        }
      }
      de265_internals_release_snapshots(img);  // as the plugin does once it has copied a picture out
      de265_release_next_picture(ctx);
      out.push_back(std::move(p));
    }
  }
  if (peak != nullptr) {
    *peak = de265_internals_get_snapshot_peak_bytes(ctx);
  }
  de265_free_decoder(ctx);
  return out;
}

// Number of samples of the crop window of @p plane (component @p c) that differ from the reference picture.
std::size_t cropped_mismatches(const FrameOut& f, const PlaneCopy& plane, int c, const RefPicture& ref) {
  const auto ci = static_cast<std::size_t>(c);
  const int sub = c == 0 ? 1 : (f.coded_w / static_cast<int>(plane.width));  // 4:2:0 -> 2 (Main / Main 10)
  const int x0 = f.crop_x / sub;
  const int y0 = f.crop_y / sub;
  REQUIRE(ref.width.at(ci) == f.crop_w / sub);
  REQUIRE(ref.height.at(ci) == f.crop_h / sub);
  REQUIRE(ref.bytes.at(ci) == plane.sample_bytes);
  std::size_t diff = 0;
  for (int y = 0; y < ref.height.at(ci); ++y) {
    for (int x = 0; x < ref.width.at(ci); ++x) {
      const std::size_t off =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(ref.width.at(ci)) + static_cast<std::size_t>(x)) *
          static_cast<std::size_t>(ref.bytes.at(ci));
      int64_t want = ref.planes.at(ci)[off];
      if (ref.bytes.at(ci) == 2) {
        want |= int64_t{ref.planes.at(ci)[off + 1]} << 8;
      }
      diff += plane.at(static_cast<uint32_t>(x0 + x), static_cast<uint32_t>(y0 + y)) != want ? 1U : 0U;
    }
  }
  return diff;
}

// AT2 streams: intra with PCM, inter (merge), SAO, transform skip, 10-bit WPP (decoded with worker threads).
constexpr std::array<std::string_view, 5> kAt2Streams = {
    "jctvc_ipcm_A_NEC_3", "jctvc_PMERGE_A_TI_3",           "jctvc_SAO_A_MediaTek_4",
    "jctvc_TSKIP_A_MS_3", "jctvc_WPP_A_ericsson_MAIN10_2",
};

}  // namespace

TEST_CASE("de265 planes: caps announce the patch-0003 stages", "[fast][third_party]") {
  const ovt_AnalysisCaps* caps = plugin_api().caps();
  REQUIRE(caps != nullptr);
  CHECK(caps->plane_stages == kAllStages);  // no PreLoopRestoration: HEVC has none
}

TEST_CASE("de265 planes: snapshot getters tolerate a null image and context", "[fast][third_party]") {
  int w = -1;
  int h = -1;
  int stride = -1;
  CHECK(de265_internals_get_snapshot_plane(nullptr, DE265_INTERNALS_SNAPSHOT_PRE_DEBLOCK, 0, &w, &h, &stride) ==
        nullptr);
  CHECK((w == 0 && h == 0 && stride == 0));
  de265_internals_release_snapshots(nullptr);
  de265_internals_set_plane_snapshots(nullptr, 1);
  CHECK(de265_internals_get_snapshot_peak_bytes(nullptr) == 0U);
}

TEST_CASE("de265 planes: de265_predeblock_vs_filters_off on ipcm_A_NEC_3", "[fast][third_party][needs-corpus]") {
  // AT1. In an intra-only stream no picture predicts from a filtered one, so a decode with both in-loop filters
  // off reconstructs exactly the pre-deblocking picture; likewise a decode with only SAO off gives PreSaoOrCdef.
  const std::vector<uint8_t> bytes = read_file(ovt::test::corpus_or_skip("jctvc_ipcm_A_NEC_3"));
  const int threads = GENERATE(0, 2);
  CAPTURE(threads);
  const std::vector<FrameOut> frames = run_plugin(bytes, threads, kAllStages);
  const std::vector<RefPicture> filters_off = run_reference(bytes, false, false);
  const std::vector<RefPicture> sao_off = run_reference(bytes, true, false);
  const std::vector<RefPicture> normal = run_reference(bytes, true, true);
  REQUIRE(!frames.empty());
  REQUIRE(frames.size() == filters_off.size());
  REQUIRE(frames.size() == sao_off.size());
  REQUIRE(frames.size() == normal.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const FrameOut& f = frames[i];
    CAPTURE(i, f.decode_index);
    REQUIRE(f.type == ovt::fb::FrameType::I);  // the premise of AT1
    REQUIRE(f.stages.size() == 6);
    for (int c = 0; c < 3; ++c) {
      CAPTURE(c);
      CHECK(cropped_mismatches(f, f.stages.at(3).at(static_cast<std::size_t>(c)), c, filters_off[i]) == 0U);
      CHECK(cropped_mismatches(f, f.stages.at(4).at(static_cast<std::size_t>(c)), c, sao_off[i]) == 0U);
      CHECK(cropped_mismatches(f, f.stages.at(0).at(static_cast<std::size_t>(c)), c, normal[i]) == 0U);
    }
  }
}

TEST_CASE("de265 planes: de265_pred_plus_resid", "[fast][third_party][needs-corpus]") {
  // AT2, on every frame (intra and inter) of each stream; the WPP stream decodes with worker threads, which takes
  // libde265's threaded filter path.
  std::size_t present = 0;
  for (const std::string_view s : kAt2Streams) {
    present += ovt::test::corpus_path(s).has_value() ? 1U : 0U;
  }
  if (present == 0) {
    (void)ovt::test::corpus_or_skip(kAt2Streams.front());  // skips, or fails under OVT_REQUIRE_CORPORA=1
  }
  const std::string_view id = GENERATE(from_range(kAt2Streams));
  CAPTURE(id);
  const auto path = ovt::test::corpus_path(id);
  REQUIRE(path.has_value());
  const std::vector<uint8_t> bytes = read_file(*path);
  const std::vector<FrameOut> frames = run_plugin(bytes, 2, kAllStages);
  const std::vector<FrameOut> recon_only = run_plugin(bytes, 2, kRecon);
  REQUIRE(!frames.empty());
  REQUIRE(frames.size() == recon_only.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const FrameOut& f = frames[i];
    CAPTURE(i, f.decode_index);
    REQUIRE(f.stages.size() == 6);  // Reconstruction and the five patch-0003 stages
    for (std::size_t c = 0; c < 3; ++c) {
      CAPTURE(c);
      const PlaneCopy& pred = f.stages.at(1).at(c);
      const PlaneCopy& resid = f.stages.at(2).at(c);
      const PlaneCopy& pre_deblock = f.stages.at(3).at(c);
      const PlaneCopy& coeff = f.stages.at(6).at(c);
      REQUIRE(
          (pred.width == pre_deblock.width && resid.width == pre_deblock.width && coeff.width == pre_deblock.width));
      REQUIRE((pred.height == pre_deblock.height && resid.height == pre_deblock.height &&
               coeff.height == pre_deblock.height));
      CHECK(resid.bit_depth == pre_deblock.bit_depth);
      std::size_t mismatches = 0;
      for (uint32_t y = 0; y < pre_deblock.height; ++y) {
        for (uint32_t x = 0; x < pre_deblock.width; ++x) {
          mismatches += pred.at(x, y) + resid.at(x, y) != pre_deblock.at(x, y) ? 1U : 0U;
        }
      }
      CHECK(mismatches == 0U);
      // Snapshots change nothing in the decoded picture.
      CHECK(f.stages.at(0).at(c).data == recon_only[i].stages.at(0).at(c).data);
    }
  }
}

TEST_CASE("de265 planes: de265_snapshots_off_memory", "[fast][third_party][needs-corpus]") {
  // AT3. Snapshots off: libde265 allocates no snapshot memory at all, so the decoder's memory is that of the
  // build without patch 0003; the plugin keeps and exports no snapshot plane.
  const std::vector<uint8_t> bytes = read_file(ovt::test::corpus_or_skip("jctvc_PMERGE_A_TI_3"));
  uint64_t peak_off = 1;
  const std::vector<RefPicture> off = run_reference(bytes, true, true, false, &peak_off);
  CHECK(peak_off == 0U);
  // On, and released when each picture is output: held only while pictures are being decoded or reordered.
  uint64_t peak_on = 0;
  const std::vector<RefPicture> on = run_reference(bytes, true, true, true, &peak_on);
  CHECK(peak_on > 0U);
  REQUIRE(!on.empty());
  const std::size_t picture_samples = on[0].planes[0].size() / static_cast<std::size_t>(on[0].bytes[0]);
  CHECK(peak_on < std::size_t{16} * 9U * picture_samples);  // < 16 pictures' worth of snapshots: freed at output
  CHECK(off.size() == on.size());

  const std::vector<FrameOut> frames = run_plugin(bytes, 0, kRecon);
  REQUIRE(!frames.empty());
  for (const FrameOut& f : frames) {
    CHECK(f.stages.size() == 1);  // Reconstruction only
  }
}

TEST_CASE("de265 planes: a late request covers the pictures decoded after it", "[fast][third_party][needs-corpus]") {
  // Snapshots are taken while a picture decodes, so a pull that first asks for a patch-0003 stage cannot give them
  // for pictures already decoded: those frames carry Reconstruction only (the host reports ANA-0001 for the missing
  // stages), and every picture that starts decoding afterwards carries all of them.
  const std::vector<uint8_t> bytes = read_file(ovt::test::corpus_or_skip("jctvc_PMERGE_A_TI_3"));
  const std::size_t units = split_annexb(bytes).size();
  const std::vector<FrameOut> frames = run_plugin(bytes, 0, kAllStages, /*prime=*/false, units / 2);
  REQUIRE(frames.size() > 2);
  std::map<int, std::size_t> stages_by_decode_index;
  for (const FrameOut& f : frames) {
    CHECK((f.stages.size() == 1 || f.stages.size() == 6));
    stages_by_decode_index[f.decode_index] = f.stages.size();
  }
  CHECK(stages_by_decode_index.begin()->second == 1U);   // decoded before the first pull
  CHECK(stages_by_decode_index.rbegin()->second == 6U);  // decoded after it
}
