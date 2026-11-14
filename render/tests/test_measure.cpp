// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.452 (chapter 10 §10.11): VideoSurface::measure() — the linear measurement texture (§10.7.1), the MaxCLL /
// FALL / min reductions and the PQ histogram with its 99.99th percentile (§10.7.2, §10.7.3), and the
// double-buffered, non-blocking readback (§10.7.9). The patterns are synthesised from their expected.json
// (tests/corpora/patterns/, WI-1.413) at full 1920x1080 size with the generator's limited-range codes, which is what
// the lossless pattern files decode to (docs/acceptance/wi/WI-4.452.md records the comparison).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>

#include <libplacebo/colorspace.h>

#include "measure/measure.h"
#include "ovt/render/video_surface.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::Measurements;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::test::OwnedPicture;
using ovt::render::test::surface_or_skip;
namespace detail = ovt::render::detail;

nlohmann::json expected(const std::string& pattern) {
  std::ifstream in(std::string(OVT_PATTERNS_DIR) + "/" + pattern + ".expected.json");
  REQUIRE(in.good());
  return nlohmann::json::parse(in);
}

// 10-bit limited BT.2020 4:2:0 picture of w x h, luma `fill`, neutral chroma.
OwnedPicture bt2020_picture(uint32_t w, uint32_t h, uint8_t transfer, uint32_t fill) {
  OwnedPicture op(w, h, 10, 1, 2);
  op.pic.color.primaries = 9;  // BT.2020
  op.pic.color.transfer = transfer;
  op.pic.color.matrix = 9;  // BT.2020 NCL
  op.fill_plane(0, fill);
  op.fill_plane(1, 512);
  op.fill_plane(2, 512);
  return op;
}

// tests/corpora/patterns/pq_patches_1000 frame 0: six equal-width full-height columns at the limited codes.
OwnedPicture pq_patches(const nlohmann::json& exp) {
  OwnedPicture op = bt2020_picture(exp.at("width").get<uint32_t>(), exp.at("height").get<uint32_t>(), 16, 64);
  const auto& columns = exp.at("patch_columns");
  const auto& codes = exp.at("codes_limited");
  for (size_t p = 0; p < columns.size(); ++p) {
    for (uint32_t y = 0; y < op.pic.coded_height; ++y) {
      for (uint32_t x = columns[p][0].get<uint32_t>(); x < columns[p][1].get<uint32_t>(); ++x) {
        op.set(0, x, y, codes[p].get<uint32_t>());
      }
    }
  }
  return op;
}

// tests/corpora/patterns/hlg_patches frame 0: 75 % and 100 % white in the top half, and the generator's ramp
// code(x) = round(876 * x / (width - 1) + 64) in the bottom half (tools/gen_hdr_patterns.py pattern_hlg_patches).
OwnedPicture hlg_patches(const nlohmann::json& exp) {
  const auto w = exp.at("width").get<uint32_t>();
  OwnedPicture op = bt2020_picture(w, exp.at("height").get<uint32_t>(), 18, 64);
  const auto& rects = exp.at("patch_rects");  // [x0, x1, y0, y1]
  const auto& codes = exp.at("patch_codes_limited");
  for (size_t p = 0; p < rects.size(); ++p) {
    for (uint32_t y = rects[p][2].get<uint32_t>(); y < rects[p][3].get<uint32_t>(); ++y) {
      for (uint32_t x = rects[p][0].get<uint32_t>(); x < rects[p][1].get<uint32_t>(); ++x) {
        op.set(0, x, y, codes[p].get<uint32_t>());
      }
    }
  }
  const auto& ramp = exp.at("ramp_rect");
  for (uint32_t y = ramp[2].get<uint32_t>(); y < ramp[3].get<uint32_t>(); ++y) {
    for (uint32_t x = ramp[0].get<uint32_t>(); x < ramp[1].get<uint32_t>(); ++x) {
      const double e = static_cast<double>(x) / static_cast<double>(w - 1);
      op.set(0, x, y, static_cast<uint32_t>(std::floor((876.0 * e) + 64.0 + 0.5)));
    }
  }
  return op;
}

ViewState one_to_one(const OwnedPicture& op) {
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = op.pic.coded_width / 2.0;
  view.pan_y = op.pic.coded_height / 2.0;
  return view;
}

bool same_frame(const ovt::fb::FrameId& a, const ovt::fb::FrameId& b) {
  return a.decode_index() == b.decode_index() && a.display_index() == b.display_index();
}

// The consumer loop of §10.7.9: measure() (asks), render() (measures), then measure() until the result for `frame`
// has come back. measure() itself never blocks.
Measurements measure_frame(VideoSurface& surface, const ViewState& view, const ovt::fb::FrameId& frame) {
  static_cast<void>(surface.measure());
  surface.render(view);
  REQUIRE(std::string(surface.last_error()).empty());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  Measurements m = surface.measure();
  while (!(m.valid && same_frame(m.measured_frame, frame)) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m = surface.measure();
  }
  REQUIRE(m.valid);
  REQUIRE(same_frame(m.measured_frame, frame));
  return m;
}

uint64_t histogram_total(const Measurements& m) {
  uint64_t total = 0;
  for (const uint32_t c : m.histogram_pq) {
    total += c;
  }
  return total;
}

void set_frame(VideoSurface& surface, OwnedPicture& op, int index, uint64_t generation) {
  op.pic.id = ovt::fb::FrameId(index, index);
  op.pic.generation = generation;
  surface.set_frame(op.pic);
  REQUIRE(std::string(surface.last_error()).empty());
}

}  // namespace

TEST_CASE("render_test_measure: subsampling, reduction chain and percentile helpers (§10.7.3)", "[fast][render]") {
  CHECK(detail::subsample_factor(0) == 1);
  CHECK(detail::subsample_factor(uint64_t{1920} * 1080) == 1);
  CHECK(detail::subsample_factor((uint64_t{1920} * 1080) + 1) == 2);
  CHECK(detail::subsample_factor(uint64_t{3840} * 2160) == 2);
  CHECK(detail::subsample_factor(uint64_t{7680} * 4320) == 4);
  CHECK(detail::sampled_extent(3840, 2) == 1920);
  CHECK(detail::sampled_extent(3841, 2) == 1921);
  CHECK(detail::sampled_extent(7, 1) == 7);

  using Chain = std::vector<std::array<int, 2>>;
  CHECK(detail::reduction_chain(3840, 2160) == Chain{{480, 270}, {60, 34}, {8, 5}, {1, 1}});  // §10.7.3: 4 passes
  CHECK(detail::reduction_chain(1920, 1080) == Chain{{240, 135}, {30, 17}, {4, 3}, {1, 1}});
  CHECK(detail::reduction_chain(1, 1) == Chain{{1, 1}});
  CHECK(detail::reduction_chain(9, 1) == Chain{{2, 1}, {1, 1}});
  CHECK(detail::reduction_chain(0, 4).empty());

  CHECK(detail::pq_oetf(10000.0) == Catch::Approx(1.0));
  CHECK(detail::pq_oetf(0.0) == Catch::Approx(0.0).margin(1e-6));
  CHECK(detail::pq_eotf_nits(1.0) == Catch::Approx(10000.0));
  CHECK(detail::pq_eotf_nits(0.0) == Catch::Approx(0.0).margin(1e-9));
  for (const double nits : {0.01, 1.0, 100.0, 203.0, 1000.0, 4000.0}) {
    CHECK(detail::pq_eotf_nits(detail::pq_oetf(nits)) == Catch::Approx(nits).epsilon(1e-9));
  }
  CHECK(detail::pq_oetf(203.0) == Catch::Approx(0.58).margin(0.001));  // BT.2408 reference white ≈ 58 % PQ

  std::array<uint32_t, detail::kHistogramBins> hist{};
  CHECK(detail::histogram_p9999_nits(hist, 0) == 0.0F);
  hist[100] = 50;
  CHECK(detail::histogram_p9999_nits(hist, 50) == Catch::Approx(detail::pq_eotf_nits(101.0 / 1024.0)));
  hist = {};
  hist[10] = 9999;  // exactly 99.99 % at bin 10
  hist[1000] = 1;
  CHECK(detail::histogram_p9999_nits(hist, 10000) == Catch::Approx(detail::pq_eotf_nits(11.0 / 1024.0)));
  hist[10] = 9998;  // 99.98 %: the percentile moves to the bright outlier
  hist[1000] = 2;
  CHECK(detail::histogram_p9999_nits(hist, 10000) == Catch::Approx(detail::pq_eotf_nits(1001.0 / 1024.0)));
}

TEST_CASE("render_test_measure: measurement colour spaces do not tone-map (§10.7.1)", "[fast][render]") {
  struct pl_color_space pq = pl_color_space_unknown;
  pq.primaries = PL_COLOR_PRIM_BT_2020;
  pq.transfer = PL_COLOR_TRC_PQ;
  const struct pl_color_space t = detail::measurement_target_color(pq);
  CHECK(t.primaries == PL_COLOR_PRIM_BT_2020);
  CHECK(t.transfer == PL_COLOR_TRC_LINEAR);
  CHECK(t.hdr.max_luma == 10000.0F);
  CHECK(t.hdr.min_luma == Catch::Approx(PL_COLOR_HDR_BLACK));  // PQ black, not a 1000:1 linear black

  struct pl_color_space hlg = pq;
  hlg.transfer = PL_COLOR_TRC_HLG;
  CHECK(detail::measurement_source_color(hlg).hdr.max_luma == 1000.0F);  // Lw = 1000 without MDCV
  CHECK(detail::measurement_target_color(hlg).hdr.max_luma == 1000.0F);  // the OOTF runs at the target peak
  CHECK(detail::measurement_target_color(hlg).transfer == PL_COLOR_TRC_LINEAR);
  hlg.hdr.max_luma = 2000.0F;
  CHECK(detail::measurement_source_color(hlg).hdr.max_luma == 2000.0F);  // a mastering peak wins
  CHECK(detail::measurement_target_color(hlg).hdr.max_luma == 2000.0F);
}

TEST_CASE("render_test_measure: AT1 PQ patches MaxCLL, percentile and FALL (FR-HDR-006)", "[fast][render][vulkan]") {
  const nlohmann::json exp = expected("pq_patches_1000");
  auto surface = surface_or_skip(16, 16);
  OwnedPicture op = pq_patches(exp);
  set_frame(*surface, op, 0, 1);
  const Measurements m = measure_frame(*surface, one_to_one(op), op.pic.id);

  const auto max_cll = exp.at("max_cll_nits").get<double>();
  const auto fall = exp.at("fall_nits").get<double>();
  const double top_band = exp.at("band_edges_nits").back().get<double>();  // Table 10.7-1 top band: >= 4000 nits
  CHECK(m.max_cll_nits == Catch::Approx(max_cll).epsilon(0.01));
  CHECK(static_cast<double>(m.max_cll_p9999_nits) >= top_band);
  CHECK(static_cast<double>(m.max_cll_p9999_nits) <= max_cll * 1.01);
  CHECK(m.fall_nits == Catch::Approx(fall).epsilon(0.01));
  const auto& decoded = exp.at("decoded_nits_from_limited_codes");
  CHECK(m.min_nits == Catch::Approx(decoded[0].get<double>()).epsilon(0.01));

  // 1080p: no subsampling; six patches -> six occupied bins of one patch area each.
  CHECK(m.sampled_pixels == 1920U * 1080U);
  CHECK(histogram_total(m) == m.sampled_pixels);
  std::map<uint32_t, uint32_t> occupied;
  for (uint32_t i = 0; i < detail::kHistogramBins; ++i) {
    if (m.histogram_pq[i] != 0) {
      occupied[i] = m.histogram_pq[i];
    }
  }
  REQUIRE(occupied.size() == 6);
  for (const auto& [bin, count] : occupied) {
    INFO("bin " << bin);
    CHECK(count == exp.at("patch_area_samples").get<uint32_t>());
  }
  CHECK(occupied.rbegin()->first == detail::kHistogramBins - 1);  // 10 000 nits = PQ 1.0, the last bin

  // Each patch alone (crop = the patch): MaxCLL = FALL = the patch's decoded nits.
  const auto& columns = exp.at("patch_columns");
  for (size_t p = 0; p < columns.size(); ++p) {
    const int x0 = columns[p][0].get<int>();
    const int x1 = columns[p][1].get<int>();
    op.pic.crop = ovt::fb::Rect(x0, 0, x1 - x0, static_cast<int>(op.pic.coded_height));
    set_frame(*surface, op, static_cast<int>(p) + 1, p + 2);
    const Measurements mp = measure_frame(*surface, one_to_one(op), op.pic.id);
    const double nits = decoded[p].get<double>();
    INFO("patch " << p << " (" << nits << " nits)");
    CHECK(mp.max_cll_nits == Catch::Approx(nits).epsilon(0.01));
    CHECK(mp.fall_nits == Catch::Approx(nits).epsilon(0.01));
    CHECK(mp.min_nits == Catch::Approx(nits).epsilon(0.01));
    CHECK(mp.sampled_pixels == static_cast<uint32_t>(x1 - x0) * op.pic.coded_height);
  }
}

TEST_CASE("render_test_measure: AT2 HLG 75 % white is 203 nits at Lw = 1000 (FR-HDR-006)", "[fast][render][vulkan]") {
  const nlohmann::json exp = expected("hlg_patches");
  auto surface = surface_or_skip(16, 16);
  OwnedPicture op = hlg_patches(exp);

  // The 75 % patch alone (crop = its rectangle): BT.2408 nominal 203 nits, ±3 %.
  const auto& r = exp.at("patch_rects")[0];
  REQUIRE(exp.at("patch_signals")[0].get<double>() == 0.75);
  op.pic.crop = ovt::fb::Rect(r[0].get<int>(), r[2].get<int>(), r[1].get<int>() - r[0].get<int>(),
                              r[3].get<int>() - r[2].get<int>());
  set_frame(*surface, op, 0, 1);
  const Measurements patch = measure_frame(*surface, one_to_one(op), op.pic.id);
  CHECK(patch.fall_nits == Catch::Approx(203.0).epsilon(0.03));
  CHECK(patch.max_cll_nits == Catch::Approx(203.0).epsilon(0.03));
  CHECK(patch.min_nits == Catch::Approx(203.0).epsilon(0.03));

  // The whole frame: MaxCLL = the 100 % patch (1000 nits) and FALL as the generator computed it, ±1 %.
  op.pic.crop = ovt::fb::Rect();
  set_frame(*surface, op, 1, 2);
  const Measurements frame = measure_frame(*surface, one_to_one(op), op.pic.id);
  CHECK(frame.max_cll_nits == Catch::Approx(exp.at("max_cll_nits").get<double>()).epsilon(0.01));
  CHECK(frame.fall_nits == Catch::Approx(exp.at("fall_nits").get<double>()).epsilon(0.01));
  CHECK(frame.min_nits <= 0.01F);  // ramp starts at code 64 = black
  CHECK(histogram_total(frame) == frame.sampled_pixels);
}

TEST_CASE("render_test_measure: readback is non-blocking and on demand (§10.7.9)", "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 16);
  CHECK_FALSE(surface->measure().valid);  // nothing rendered yet

  OwnedPicture first = bt2020_picture(64, 32, 16, 509);  // 100 nits (PQ limited code 509)
  set_frame(*surface, first, 7, 1);
  const ViewState view = one_to_one(first);

  // No measure() within the last two renders: render() skips the passes, so nothing ever arrives.
  auto idle = VideoSurface::create("render-test-idle");
  ovt::render::DisplayTarget t;
  t.width = 16;
  t.height = 16;
  REQUIRE(idle->init(t));
  idle->set_frame(first.pic);
  for (int i = 0; i < 3; ++i) {
    idle->render(view);
  }
  ovt::render::detail::OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(*idle, px));  // waits for the GPU
  for (int i = 0; i < 20; ++i) {
    CHECK_FALSE(idle->measure().valid);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  idle.reset();

  const Measurements a = measure_frame(*surface, view, first.pic.id);
  CHECK(a.max_cll_nits == Catch::Approx(99.91).epsilon(0.01));

  // A new frame: the first measure() after the change still reports the previous frame, named by measured_frame.
  OwnedPicture second = bt2020_picture(64, 32, 16, 723);  // 1004 nits
  set_frame(*surface, second, 8, 2);
  const Measurements stale = surface->measure();
  REQUIRE(stale.valid);
  CHECK(same_frame(stale.measured_frame, first.pic.id));
  const Measurements b = measure_frame(*surface, view, second.pic.id);
  CHECK(b.max_cll_nits == Catch::Approx(1004.19).epsilon(0.01));
  CHECK(b.gpu_time_ms == 0.0);  // no GPU timers in libovt-render (vk_no_timestamps.cpp)
}

TEST_CASE("render_test_measure: subsampled histogram, exact MaxCLL, deterministic (§10.7.3)",
          "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 16);
  // 2048 x 1024 = 2 097 152 pixels > 2 073 600: s = 2, so the histogram samples 1024 x 512 pixels. One bright
  // pixel at an odd position is off the sample grid: MaxCLL still sees it, the histogram does not.
  OwnedPicture op = bt2020_picture(2048, 1024, 16, 509);
  op.set(0, 1001, 501, 940);  // 10 000 nits
  set_frame(*surface, op, 3, 1);
  const ViewState view = one_to_one(op);
  const Measurements m = measure_frame(*surface, view, op.pic.id);
  CHECK(m.sampled_pixels == 1024U * 512U);
  CHECK(histogram_total(m) == m.sampled_pixels);
  CHECK(m.max_cll_nits == Catch::Approx(10000.0).epsilon(0.01));
  CHECK(m.max_cll_p9999_nits < 110.0F);  // the lone bright pixel is not sampled
  // FALL is over every pixel: (2 097 151 x 99.91 + 10 000) / 2 097 152.
  CHECK(m.fall_nits == Catch::Approx(((2097151.0 * 99.9128) + 10000.0) / 2097152.0).epsilon(0.01));

  // Same frame again (new generation forces a re-upload and a new measurement): identical results.
  set_frame(*surface, op, 4, 2);
  const Measurements again = measure_frame(*surface, view, op.pic.id);
  CHECK(again.max_cll_nits == m.max_cll_nits);
  CHECK(again.fall_nits == m.fall_nits);
  CHECK(again.min_nits == m.min_nits);
  CHECK(again.max_cll_p9999_nits == m.max_cll_p9999_nits);
  CHECK(std::memcmp(static_cast<const void*>(again.histogram_pq), static_cast<const void*>(m.histogram_pq),
                    sizeof(m.histogram_pq)) == 0);
}
