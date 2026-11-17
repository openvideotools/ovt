// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.437 (chapter 10 §10.11): the overlay cache (LRU of 8 built OverlaySets per surface, byte budget) and the
// set_overlays() lifecycle. The cache tests need no GPU: a VideoSurface that was never init()ed owns its cache and
// set_overlays() still adopts sets into it. The [vulkan] cases check the shared-array re-upload skip and teardown.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fixtures/make_synthetic.h"
#include "overlay/overlay_cache.h"
#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"
#include "ovt_model_generated.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

namespace fb = ovt::fb;
using ovt::render::HeatmapSource;
using ovt::render::OverlayBuilder;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::OverlaySet;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::OverlayCache;
using ovt::render::detail::OverlayCacheKey;
using ovt::render::test::SyntheticOptions;

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

// The synthetic fixture (80 x 64 cells) as frame `decode_index` of a stream; `pts` stands for another stream.
std::vector<uint8_t> frame(int32_t decode_index, int64_t pts = 0, const SyntheticOptions& opts = {}) {
  const std::vector<uint8_t> base = ovt::render::test::make_synthetic(opts);
  const std::unique_ptr<fb::FrameAnalysisT> fa = fb::UnPackFrameAnalysis(base.data());
  fa->frame = std::make_unique<fb::FrameInfoT>();
  fa->frame->id = std::make_unique<fb::FrameId>(decode_index, decode_index);
  fa->frame->pts = pts;
  flatbuffers::FlatBufferBuilder fbb;
  fb::FinishFrameAnalysisBuffer(fbb, fb::FrameAnalysis::Pack(fbb, fa.get()));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

OverlayConfig fill_and_mv() {
  OverlayConfig cfg;
  cfg.layers = bit(OverlayLayer::PredFill) | bit(OverlayLayer::MvL0);
  return cfg;
}

std::unique_ptr<VideoSurface> bare_surface() {
  auto surface = VideoSurface::create("overlay-cache-test");
  REQUIRE(surface != nullptr);
  REQUIRE(ovt::render::detail::overlay_cache(*surface) != nullptr);
  return surface;
}

std::unique_ptr<OverlaySet> build(OverlayBuilder& builder, const std::vector<uint8_t>& fa, const OverlayConfig& cfg,
                                  const uint16_t* custom = nullptr) {
  auto set = builder.build(fa.data(), fa.size(), cfg, custom);
  REQUIRE(set != nullptr);
  return set;
}

bool is_hit(const OverlaySet& set) {
  return set.shared != nullptr;
}

}  // namespace

TEST_CASE("render_test_overlay_cache_lru (AT1): frame 1 of 10 is evicted, frame 10 is a hit",
          "[fast][render][overlay]") {
  auto surface = bare_surface();
  const OverlayCache& cache = *ovt::render::detail::overlay_cache(*surface);
  OverlayBuilder builder(*surface);
  const OverlayConfig cfg = fill_and_mv();
  std::vector<std::vector<uint8_t>> frames;
  for (int32_t i = 1; i <= 10; ++i) {
    frames.push_back(frame(i));
  }
  // Given 10 frames built in sequence (and shown, which is when a built set joins the cache).
  for (const auto& fa : frames) {
    auto set = build(builder, fa, cfg);
    CHECK_FALSE(is_hit(*set));
    CHECK(set->cache_tagged);
    surface->set_overlays(std::move(set));
  }
  CHECK(cache.size() == ovt::render::detail::kOverlayCacheEntries);
  CHECK(cache.misses() == 10);
  CHECK(cache.hits() == 0);

  // When frame 1 is requested again, Then it is rebuilt (evicted) ...
  auto again = build(builder, frames.front(), cfg);
  CHECK_FALSE(is_hit(*again));
  CHECK(cache.misses() == 11);
  CHECK(again->cell_attributes.size() == size_t{80} * 64 * 4);
  surface->set_overlays(std::move(again));
  CHECK(cache.size() == ovt::render::detail::kOverlayCacheEntries);

  // ... and frame 10 is a hit, sharing the arrays built the first time.
  auto ten = build(builder, frames.back(), cfg);
  REQUIRE(is_hit(*ten));
  CHECK(cache.hits() == 1);
  CHECK(ten->data().cell_attributes.size() == size_t{80} * 64 * 4);
  CHECK(ten->cell_attributes.empty());  // a handle owns no arrays of its own

  // Showing frames 1..10 left 3..10 (the set on screen counts as one of the 8); showing frame 1 again evicted the
  // least recently used one, frame 3. Frame 4 is still there.
  CHECK_FALSE(is_hit(*build(builder, frames.at(1), cfg)));
  CHECK_FALSE(is_hit(*build(builder, frames.at(2), cfg)));
  CHECK(is_hit(*build(builder, frames.at(3), cfg)));
}

TEST_CASE("render_test_overlay_cache_key: uniforms are not in the key, mv_scale and layers are",
          "[fast][render][overlay]") {
  auto surface = bare_surface();
  OverlayBuilder builder(*surface);
  const std::vector<uint8_t> fa = frame(7);
  const OverlayConfig base = fill_and_mv();
  surface->set_overlays(build(builder, fa, base));

  SECTION("selection, probe, selection_rect and draw-time uniforms hit, with the requested configuration") {
    OverlayConfig moved = base;
    moved.layers |= bit(OverlayLayer::Selection) | bit(OverlayLayer::Probe) | bit(OverlayLayer::SelectionRect);
    moved.selected_block = 1;
    moved.hovered_block = 0;
    moved.selected_mv = 12;
    moved.selection_rect = fb::Rect(4, 4, 16, 16);
    moved.probe_x = 10;
    moved.probe_y = 20;
    moved.heatmap_alpha = 0.25F;
    moved.line_px = 2.0F;
    moved.mv_min_px = 4.0F;
    moved.label_zoom_threshold = 8.0F;
    auto set = build(builder, fa, moved);
    REQUIRE(is_hit(*set));
    CHECK(set->config.selected_block == 1);
    CHECK(set->config.hovered_block == 0);
    // The WI-2.436 marks read block_rects / mv_rects through data(): a handle owns none of its own.
    CHECK(set->block_rects.empty());
    CHECK(set->data().block_rects.size() == 2);
    CHECK(set->config.probe_x == 10);
    CHECK(set->config.heatmap_alpha == 0.25F);
    CHECK(set->config.layers == moved.layers);
    CHECK(set->data().config.selected_block == base.selected_block);  // the shared arrays keep the first build's
  }
  SECTION("mv_scale is baked into the arrow instances, so it misses") {
    OverlayConfig scaled = base;
    scaled.mv_scale = 2.0F;
    auto set = build(builder, fa, scaled);
    CHECK_FALSE(is_hit(*set));
    CHECK(ovt::render::detail::overlay_config_hash(scaled) != ovt::render::detail::overlay_config_hash(base));
  }
  SECTION("toggling a layer off and on again: the second toggle is a hit") {
    OverlayConfig more = base;
    more.layers |= bit(OverlayLayer::GridCU);
    CHECK_FALSE(is_hit(*build(builder, fa, more)));
    surface->set_overlays(build(builder, fa, more));  // a miss again: a build joins the cache when it is shown
    CHECK(is_hit(*build(builder, fa, base)));
    CHECK(is_hit(*build(builder, fa, more)));
  }
  SECTION("heatmap source and range are in the key") {
    OverlayConfig heat = base;
    heat.layers |= bit(OverlayLayer::Heatmap);
    heat.heatmap = HeatmapSource::QP;
    surface->set_overlays(build(builder, fa, heat));
    CHECK(is_hit(*build(builder, fa, heat)));
    OverlayConfig ranged = heat;
    ranged.heatmap_max = 40.0F;
    CHECK_FALSE(is_hit(*build(builder, fa, ranged)));
  }
}

TEST_CASE("render_test_overlay_cache_key: another stream or other host cells do not alias", "[fast][render][overlay]") {
  auto surface = bare_surface();
  OverlayBuilder builder(*surface);
  const OverlayConfig cfg = fill_and_mv();
  surface->set_overlays(build(builder, frame(3, /*pts=*/3000), cfg));

  // decode_index 3 of another stream (other pts) is rebuilt.
  CHECK_FALSE(is_hit(*build(builder, frame(3, /*pts=*/9000), cfg)));
  CHECK(is_hit(*build(builder, frame(3, /*pts=*/3000), cfg)));

  // Custom heatmap cells are part of the key; their contents, not their address.
  OverlayConfig custom = cfg;
  custom.layers |= bit(OverlayLayer::Heatmap);
  custom.heatmap = HeatmapSource::Custom;
  std::vector<uint16_t> cells(size_t{80} * 64, 100);
  const std::vector<uint8_t> fa = frame(4);
  surface->set_overlays(build(builder, fa, custom, cells.data()));
  CHECK(is_hit(*build(builder, fa, custom, cells.data())));
  const std::vector<uint16_t> same = cells;
  CHECK(is_hit(*build(builder, fa, custom, same.data())));
  cells.at(17) = 200;
  CHECK_FALSE(is_hit(*build(builder, fa, custom, cells.data())));
}

TEST_CASE("render_test_overlay_cache: frames without FrameInfo.id are never cached", "[fast][render][overlay]") {
  auto surface = bare_surface();
  OverlayBuilder builder(*surface);
  const std::vector<uint8_t> fa = ovt::render::test::make_synthetic();  // no FrameInfo
  auto set = build(builder, fa, fill_and_mv());
  CHECK_FALSE(set->cache_tagged);
  surface->set_overlays(std::move(set));
  CHECK(ovt::render::detail::overlay_cache(*surface)->size() == 0);
  CHECK_FALSE(is_hit(*build(builder, fa, fill_and_mv())));
}

TEST_CASE("render_test_overlay_cache_budget: byte budget and eviction keep shown arrays alive",
          "[fast][render][overlay]") {
  const std::vector<uint8_t> fa = frame(1);
  auto make = [&](int32_t index) {
    // build_cached without a cache is the plain build (build_overlay_set is not exported from libovt-render).
    auto set = ovt::render::detail::build_cached(nullptr, fb::GetFrameAnalysis(fa.data()), fill_and_mv(), nullptr);
    REQUIRE(set != nullptr);
    OverlayCacheKey key;
    REQUIRE(ovt::render::detail::overlay_cache_key(*fb::GetFrameAnalysis(fa.data()), fill_and_mv(), nullptr, key));
    key.decode_index = index;
    return std::make_pair(key, std::shared_ptr<const OverlaySet>(std::move(set)));
  };
  const auto first = make(1);
  const size_t one = ovt::render::detail::overlay_set_bytes(*first.second);
  REQUIRE(one > 0);

  SECTION("the budget evicts before the entry count does") {
    OverlayCache cache(8, (3 * one) + (one / 2));  // room for 3 sets
    auto shown = make(1);
    const std::weak_ptr<const OverlaySet> oldest = shown.second;
    cache.insert(shown.first, shown.second);
    std::shared_ptr<const OverlaySet> held = std::move(shown.second);  // the handle on screen
    for (int32_t i = 2; i <= 5; ++i) {
      const auto e = make(i);
      cache.insert(e.first, e.second);
    }
    CHECK(cache.size() == 3);
    CHECK(cache.bytes() <= (3 * one) + (one / 2));
    CHECK(cache.find(first.first) == nullptr);
    CHECK_FALSE(oldest.expired());  // evicted, but the handle on screen keeps the arrays
    held.reset();
    CHECK(oldest.expired());
  }
  SECTION("a set larger than the whole budget is not kept") {
    OverlayCache cache(8, one / 2);
    cache.insert(first.first, first.second);
    CHECK(cache.size() == 0);
    CHECK(cache.bytes() == 0);
  }
  SECTION("re-inserting a key replaces it; clear() empties the cache") {
    OverlayCache cache;
    cache.insert(first.first, first.second);
    cache.insert(first.first, make(1).second);
    CHECK(cache.size() == 1);
    CHECK(cache.bytes() == one);
    cache.clear();
    CHECK(cache.size() == 0);
    CHECK(cache.bytes() == 0);
  }
  CHECK(ovt::render::detail::kOverlayCacheEntries == 8);
#if defined(__ANDROID__)
  CHECK(ovt::render::detail::kOverlayCacheBudgetBytes == size_t{64} << 20U);
#else
  CHECK(ovt::render::detail::kOverlayCacheBudgetBytes == size_t{256} << 20U);
#endif
}

TEST_CASE("render_test_overlay_cache GPU: a cache hit draws its own uniforms; teardown with live handles",
          "[fast][render][vulkan][overlay]") {
  constexpr int kW = 320;  // the synthetic fixture: 80 x 64 cells = 320 x 256 luma samples, drawn 1:1
  constexpr int kH = 256;
  ovt::render::test::OwnedPicture pic(kW, kH, 8, 1, 1);
  pic.fill_plane(0, 126);
  pic.fill_plane(1, 128);
  pic.fill_plane(2, 128);
  pic.pic.generation = 1;
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = kW / 2.0;
  view.pan_y = kH / 2.0;
  view.dither = false;

  OverlayConfig strong;
  strong.layers = bit(OverlayLayer::Heatmap);
  strong.heatmap = HeatmapSource::QP;
  strong.heatmap_alpha = 1.0F;
  OverlayConfig faint = strong;
  faint.heatmap_alpha = 0.2F;
  const std::vector<uint8_t> fa = frame(5);

  auto render = [&](VideoSurface& surface, std::unique_ptr<OverlaySet> set) {
    surface.set_overlays(std::move(set));
    REQUIRE(std::string(surface.last_error()).empty());
    surface.render(view);
    REQUIRE(std::string(surface.last_error()).empty());
    ovt::render::detail::OutputPixels px;
    REQUIRE(ovt::render::detail::download_output(surface, px));
    return px.bytes;
  };

  // Reference: each alpha built fresh on its own surface (a set without FrameInfo is never cached).
  std::vector<uint8_t> want_faint;
  {
    auto ref = ovt::render::test::surface_or_skip(kW, kH);
    ref->set_frame(pic.pic);
    OverlayBuilder b(*ref);
    const std::vector<uint8_t> plain = ovt::render::test::make_synthetic();
    want_faint = render(*ref, build(b, plain, faint));
  }

  auto surface = ovt::render::test::surface_or_skip(kW, kH);
  surface->set_frame(pic.pic);
  OverlayBuilder builder(*surface);
  const std::vector<uint8_t> strong_px = render(*surface, build(builder, fa, strong));
  auto faint_set = build(builder, fa, faint);
  REQUIRE(is_hit(*faint_set));  // only heatmap_alpha changed: the arrays on the GPU are reused
  const std::vector<uint8_t> faint_px = render(*surface, std::move(faint_set));
  CHECK(faint_px != strong_px);
  CHECK(faint_px == want_faint);
  // pick() reads the block ids through the handle (WI-2.435 + WI-2.437): cell 79 of the fixture is in block 1.
  CHECK(surface->pick(kW - 2, 10).block_id == 1);
  CHECK(surface->pick(2, 10).block_id == 0);
  // Back to the first alpha: a hit again, and the same pixels as the fresh build.
  CHECK(render(*surface, build(builder, fa, strong)) == strong_px);

  // Teardown: a handle built from the surface's cache outlives the surface; the arrays are CPU memory only.
  std::unique_ptr<OverlaySet> survivor = build(builder, fa, strong);
  REQUIRE(is_hit(*survivor));
  surface->set_overlays(build(builder, frame(6), strong));
  surface.reset();
  CHECK(survivor->data().heatmap.size() == size_t{80} * 64);
  survivor.reset();
}
