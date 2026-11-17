// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.432 (chapter 10 §10.11): grid line pass with viewport and level culling (§10.5.1, §10.5.3, §10.5.6 passes
// 5-6, R-GRID-1), on the 4K partition-grid fixture of §10.9.2 item 4.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "fixtures/make_synthetic.h"
#include "overlay/overlay_set_impl.h"
#include "overlay/pass_lines.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"
#include "png_io.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::GridLevel;
using ovt::render::OverlayBuilder;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::OverlaySet;
using ovt::render::ViewState;
using ovt::render::detail::build_grid_lines;
using ovt::render::detail::expand_margin;
using ovt::render::detail::grid_levels_drawn;
using ovt::render::detail::GridLineCache;
using ovt::render::detail::GridLines;
using ovt::render::detail::ImageRect;
using ovt::render::detail::OutputPixels;
using ovt::render::detail::visible_image_rect;
using ovt::render::test::GridFixtureOptions;
using ovt::render::test::OwnedPicture;
using ovt::render::test::RgbImage;

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

constexpr uint32_t kAllGrids =
    bit(OverlayLayer::GridTU) | bit(OverlayLayer::GridPU) | bit(OverlayLayer::GridCU) | bit(OverlayLayer::GridCTU);
constexpr size_t kTu = 0;
constexpr size_t kPu = 1;
constexpr size_t kCu = 2;
constexpr size_t kCtu = 3;
constexpr size_t kPartition = 4;

std::unique_ptr<OverlaySet> build_set(const GridFixtureOptions& opts, uint32_t layers) {
  const std::vector<uint8_t> fa = ovt::render::test::make_grid_fixture(opts);
  auto surface = ovt::render::VideoSurface::create("grid-test");
  REQUIRE(surface != nullptr);
  OverlayBuilder builder(*surface);
  OverlayConfig cfg;
  cfg.layers = layers;
  auto set = builder.build(fa.data(), fa.size(), cfg);
  REQUIRE(set != nullptr);
  return set;
}

GridLines lines_for(const OverlaySet& set, uint32_t layers, int w, int h, double pan_x, double pan_y, double zoom) {
  GridLines out;
  const uint32_t levels = grid_levels_drawn(set, layers, zoom);
  build_grid_lines(set, levels, (layers & bit(OverlayLayer::GridPartition)) != 0,
                   expand_margin(visible_image_rect(w, h, pan_x, pan_y, zoom)), out);
  return out;
}

// Unit edges (4 luma samples long) of the segments of one level, as (x, y, vertical) keys.
using UnitEdge = std::tuple<int, int, bool>;

std::vector<UnitEdge> unit_edges(const GridLines& lines, size_t first, size_t count) {
  std::vector<UnitEdge> out;
  for (size_t v = first; v < first + count; v += 2) {
    const auto& a = lines.vertices[v];
    const auto& b = lines.vertices[v + 1];
    const bool vertical = a.x == b.x;
    const int from = static_cast<int>(vertical ? a.y : a.x);
    const int to = static_cast<int>(vertical ? b.y : b.x);
    REQUIRE(from < to);  // endpoints ordered, never degenerate
    for (int p = from; p < to; p += 4) {
      out.emplace_back(vertical ? static_cast<int>(a.x) : p, vertical ? p : static_cast<int>(a.y), vertical);
    }
  }
  return out;
}

size_t level_offset(const GridLines& lines, size_t level) {
  return std::accumulate(lines.level_vertices.begin(),
                         lines.level_vertices.begin() + static_cast<std::ptrdiff_t>(level), size_t{0});
}

}  // namespace

// Acceptance test 1. SPEC-ASSUMPTION(#624): the left/top rule emits 2 lines per CTU plus the right/bottom picture
// boundary lines, 2 x (2 x 2040 + 34 + 60) = 8348 vertices for the 60 x 34 CTUs of 64, not "8160 x 8".
TEST_CASE("render_test_grid AT1: fit zoom culls TU/PU/CU and emits each CTU edge once (§10.5.3)", "[fast][render]") {
  GridFixtureOptions opts;
  opts.tus = true;  // the fixture gains a TU level, so the R-GRID-1 cull of TUs is exercised, not vacuous
  const auto set = build_set(opts, kAllGrids);
  REQUIRE(set->grid_min_side.at(kCtu) == 64);
  REQUIRE(set->grid_min_side.at(kCu) == 8);
  REQUIRE(set->grid_min_side.at(kTu) == 4);

  // Fit zoom: the 3840 x 2160 picture in a 1920 x 1080 viewport.
  const GridLines lines = lines_for(*set, kAllGrids, 1920, 1080, 1920.0, 1080.0, 0.5);
  CHECK(lines.level_vertices.at(kTu) == 0);
  CHECK(lines.level_vertices.at(kPu) == 0);
  CHECK(lines.level_vertices.at(kCu) == 0);  // smallest CU 8 samples = 4 px < 6 px
  constexpr uint32_t kCtus = 60 * 34;
  CHECK(lines.level_vertices.at(kCtu) == 2 * ((2 * kCtus) + 34 + 60));
  CHECK(lines.level_vertices.at(kCtu) == 8348);
  CHECK(lines.vertices.size() == 8348);
  for (const auto& v : lines.vertices) {
    CHECK(v.color_idx == ovt::render::detail::kColorGridCtu);
  }
  // Every CTU edge is present once: no unit edge twice, and the bottom row is clipped to the picture (y = 2160).
  const auto edges = unit_edges(lines, 0, lines.vertices.size());
  const std::set<UnitEdge> unique(edges.begin(), edges.end());
  CHECK(unique.size() == edges.size());
  CHECK(unique.contains({0, 2160, false}));
  CHECK(unique.contains({3840, 0, true}));
  CHECK_FALSE(unique.contains({0, 2176, false}));
}

TEST_CASE("render_test_grid AT2: zoom 2 in a 1080p viewport stays within 480,000 vertices (§10.5.1)",
          "[fast][render]") {
  GridFixtureOptions opts;
  opts.tus = true;
  const auto set = build_set(opts, kAllGrids);
  for (const auto& [px, py] : {std::pair{1920.0, 1080.0}, std::pair{0.0, 0.0}, std::pair{3840.0, 2160.0}}) {
    const GridLines lines = lines_for(*set, kAllGrids, 1920, 1080, px, py, 2.0);
    INFO("pan " << px << "," << py);
    CHECK(lines.vertices.size() <= 480000);
    CHECK(lines.level_vertices.at(kTu) > 0);  // TU 4 samples = 8 px >= 6 px
    CHECK(lines.level_vertices.at(kCu) > 0);
    CHECK(lines.level_vertices.at(kCtu) > 0);
    // Viewport culling: every emitted line touches the area (the visible rect plus 25 % per side).
    const ImageRect area = lines.area;
    for (size_t v = 0; v < lines.vertices.size(); v += 2) {
      const auto& a = lines.vertices[v];
      const auto& b = lines.vertices[v + 1];  // a CTU edge may reach 64 samples past a block-intersected area
      REQUIRE((b.x >= area.x0 - 64 && a.x <= area.x1 + 64 && b.y >= area.y0 - 64 && a.y <= area.y1 + 64));
    }
  }
}

TEST_CASE("render_test_grid left/top rule: every block edge exactly once per level (§10.5.3)", "[fast][render]") {
  GridFixtureOptions opts;
  opts.width = 328;  // not a multiple of 64 or 8: edge CUs are split and right-column CTUs are clipped
  opts.height = 200;
  opts.tus = true;
  const auto set = build_set(opts, kAllGrids);
  const GridLines lines = lines_for(*set, kAllGrids, 1312, 800, 164.0, 100.0, 4.0);
  // Draw order of §10.5.6: TU, PU, CU, CTU.
  REQUIRE(lines.level_vertices.at(kPu) == 0);
  for (const size_t level : {kTu, kCu, kCtu}) {
    INFO("level " << level);
    const auto edges = unit_edges(lines, level_offset(lines, level), lines.level_vertices.at(level));
    const std::set<UnitEdge> unique(edges.begin(), edges.end());
    CHECK(unique.size() == edges.size());
    // Expected: the boundary of every block of the level, clipped to the picture, as unit edges.
    std::set<UnitEdge> expected;
    for (const auto& n : set->grid_nodes) {
      if (static_cast<size_t>(n.level) != level || n.level == GridLevel::None) {
        continue;
      }
      for (int x = n.x0; x < n.x1; x += 4) {
        expected.emplace(x, n.y0, false);
        expected.emplace(x, n.y1, false);
      }
      for (int y = n.y0; y < n.y1; y += 4) {
        expected.emplace(n.x0, y, true);
        expected.emplace(n.x1, y, true);
      }
    }
    CHECK(unique == expected);
  }
  // Tree order: the list is deterministic.
  const GridLines again = lines_for(*set, kAllGrids, 1312, 800, 164.0, 100.0, 4.0);
  REQUIRE(again.vertices.size() == lines.vertices.size());
  for (size_t i = 0; i < lines.vertices.size(); ++i) {
    REQUIRE(again.vertices[i].x == lines.vertices[i].x);
    REQUIRE(again.vertices[i].y == lines.vertices[i].y);
  }
}

TEST_CASE("render_test_grid partition lines (§10.5.1)", "[fast][render]") {
  GridFixtureOptions opts;
  opts.width = 256;
  opts.height = 128;
  opts.tiles_2x2 = true;
  const uint32_t layers = bit(OverlayLayer::GridCTU) | bit(OverlayLayer::GridPartition);
  const auto set = build_set(opts, layers);
  const GridLines lines = lines_for(*set, layers, 256, 128, 128.0, 64.0, 1.0);
  // 4 tiles: outer 4 edges split in halves (8) + 2 inner halves each way (4) = 12 segments, each once.
  CHECK(lines.level_vertices.at(kPartition) == 24);
  for (size_t v = level_offset(lines, kPartition); v < lines.vertices.size(); ++v) {
    CHECK(lines.vertices[v].color_idx == ovt::render::detail::kColorPartition);
  }
  // A layer mask without grid bits keeps no block tree.
  const auto no_grid = build_set(opts, bit(OverlayLayer::GridPartition));
  CHECK(no_grid->grid_nodes.empty());
}

// WI-2.432 step 5: rebuild on a level threshold crossing or a pan beyond the 25 % margin, not on every frame.
TEST_CASE("render_test_grid rebuild triggers (§10.5.1)", "[fast][render]") {
  const auto set = build_set({}, kAllGrids);
  GridLineCache cache;
  const auto vis = [](double px, double py, double zoom) { return visible_image_rect(1920, 1080, px, py, zoom); };
  CHECK(cache.update(*set, kAllGrids, 1.0, vis(1920, 1080, 1.0)));
  CHECK(cache.lines().levels == 0b1100);                                  // CU and CTU at zoom 1
  CHECK_FALSE(cache.update(*set, kAllGrids, 1.0, vis(1920, 1080, 1.0)));  // same frame
  CHECK_FALSE(cache.update(*set, kAllGrids, 1.0, vis(2300, 1250, 1.0)));  // pan inside the margin (480 x 270)
  CHECK(cache.update(*set, kAllGrids, 1.0, vis(2500, 1080, 1.0)));        // beyond it
  CHECK_FALSE(cache.update(*set, kAllGrids, 1.2, vis(2500, 1080, 1.2)));  // zoom in, no threshold crossed
  CHECK(cache.update(*set, kAllGrids, 0.5, vis(2500, 1080, 0.5)));        // CU threshold crossed (8 x 0.5 < 6)
  CHECK(cache.lines().levels == 0b1000);
  CHECK(cache.update(*set, kAllGrids | bit(OverlayLayer::GridPartition), 0.5, vis(2500, 1080, 0.5)));  // layers
  const auto other = build_set({}, kAllGrids);
  CHECK(cache.update(*other, kAllGrids | bit(OverlayLayer::GridPartition), 0.5, vis(2500, 1080, 0.5)));  // new set
  CHECK(cache.builds() == 5);
}

namespace {

RgbImage to_rgb(const OutputPixels& px) {
  RgbImage img;
  img.width = px.width;
  img.height = px.height;
  img.rgb.reserve(static_cast<size_t>(px.width) * static_cast<size_t>(px.height) * 3);
  for (size_t i = 0; i + 3 < px.bytes.size(); i += 4) {
    img.rgb.insert(img.rgb.end(), px.bytes.begin() + static_cast<std::ptrdiff_t>(i),
                   px.bytes.begin() + static_cast<std::ptrdiff_t>(i + 3));
  }
  return img;
}

std::array<int, 3> rgb_at(const OutputPixels& px, int x, int y) {
  const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(px.width)) + static_cast<size_t>(x)) * 4;
  return {px.bytes.at(i), px.bytes.at(i + 1), px.bytes.at(i + 2)};
}

// §10.9.2 item 4 comparison rule (as in test_overlay_layer.cpp, #469).
void check_golden(const RgbImage& actual, const std::string& name) {
  const std::string golden = std::string(OVT_RENDER_GOLDEN_DIR) + "/" + name;
  const char* update = std::getenv("OVT_UPDATE_GOLDEN");  // NOLINT(concurrency-mt-unsafe): test setup
  const char* ci = std::getenv("CI");                     // NOLINT(concurrency-mt-unsafe)
  if (update != nullptr && std::string(update) == "1" && (ci == nullptr || std::string(ci).empty())) {
    REQUIRE(ovt::render::test::write_png(golden, actual, /*compress=*/true));  // 6 MB stored
    WARN("rewrote golden " << golden);
    return;
  }
  const std::optional<RgbImage> read = ovt::render::test::read_png(golden);
  REQUIRE(read.has_value());
  const RgbImage expected = read.value_or(RgbImage{});
  REQUIRE(expected.width == actual.width);
  REQUIRE(expected.height == actual.height);
  size_t differing = 0;
  for (size_t p = 0; p < actual.rgb.size(); p += 3) {
    for (size_t c = 0; c < 3; ++c) {
      if (std::abs(int{actual.rgb.at(p + c)} - int{expected.rgb.at(p + c)}) > 2) {
        ++differing;
        break;
      }
    }
  }
  const size_t pixels = actual.rgb.size() / 3;
  if (differing * 1000 > pixels) {
    const std::string out = name.substr(0, name.size() - 4) + ".actual.png";
    static_cast<void>(ovt::render::test::write_png(out, actual, /*compress=*/true));
    FAIL(differing << " of " << pixels << " pixels differ from " << golden << "; this render: " << out);
  }
}

OutputPixels render_grid(ovt::render::VideoSurface& surface, const std::vector<uint8_t>& fa, uint32_t layers,
                         const ViewState& view) {
  if (layers != 0) {
    OverlayBuilder builder(surface);
    OverlayConfig cfg;
    cfg.layers = layers;
    auto set = builder.build(fa.data(), fa.size(), cfg);
    REQUIRE(set != nullptr);
    surface.set_overlays(std::move(set));
  } else {
    surface.set_overlays(nullptr);
  }
  REQUIRE(std::string(surface.last_error()).empty());
  surface.render(view);
  REQUIRE(std::string(surface.last_error()).empty());
  OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(surface, px));
  return px;
}

}  // namespace

// §10.9.2 item 4 / FR-BLK-017: synthetic_4k_grid.png, 1920 x 1080 viewport, dpr 1.0, at zoom 1 on the picture centre so
// that CU lines (1 px), CTU lines (2 px) and the dashed tile boundaries are all drawn.
TEST_CASE("render_test_grid synthetic_4k_grid golden (§10.5.3, §10.9.2)", "[fast][render][vulkan][overlay]") {
  OwnedPicture pic(3840, 2160, 8, 1, 1);
  pic.fill_plane(0, 126);
  pic.fill_plane(1, 128);
  pic.fill_plane(2, 128);
  pic.pic.generation = 1;
  auto surface = ovt::render::test::surface_or_skip(1920, 1080);
  surface->set_frame(pic.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  GridFixtureOptions opts;
  opts.tiles_2x2 = true;
  const std::vector<uint8_t> fa = ovt::render::test::make_grid_fixture(opts);
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = 1920.0;  // device x = image x - 960, device y = image y - 540
  view.pan_y = 1080.0;
  view.dither = false;
  const uint32_t layers = bit(OverlayLayer::GridCU) | bit(OverlayLayer::GridCTU) | bit(OverlayLayer::GridPartition);
  const OutputPixels first = render_grid(*surface, fa, layers, view);
  const OutputPixels second = render_grid(*surface, fa, layers, view);
  CHECK(first.bytes == second.bytes);
  check_golden(to_rgb(first), "synthetic_4k_grid.png");
  const OutputPixels plain = render_grid(*surface, fa, 0, view);

  // CTU (16, 9) has 32 x 32 CUs ((16 + 9) % 4 == 1): image (1024..1088, 576..640) -> device (64..128, 36..100).
  const auto cu_line = rgb_at(first, 106, 68);  // top edge of CU (1056, 608), 1 px yellow at A 0xCC
  CHECK(cu_line != rgb_at(plain, 106, 68));
  CHECK(cu_line[0] > cu_line[2] + 100);
  CHECK(cu_line[1] > cu_line[2] + 100);
  // Junction (1056, 608) -> device (96, 68) and the vertical edge below it are covered once: same colour as mid-line.
  CHECK(rgb_at(first, 96, 68) == cu_line);
  CHECK(rgb_at(first, 96, 80) == cu_line);
  CHECK(rgb_at(first, 100, 80) == rgb_at(plain, 100, 80));  // inside a CU
  // The compressed PNG round-trips exactly (png_io.h fixed-Huffman writer and reader).
  const std::string roundtrip = "synthetic_4k_grid.roundtrip.png";
  REQUIRE(ovt::render::test::write_png(roundtrip, to_rgb(first), /*compress=*/true));
  const std::optional<RgbImage> back = ovt::render::test::read_png(roundtrip);
  REQUIRE(back.has_value());
  CHECK(back.value_or(RgbImage{}).rgb == to_rgb(first).rgb);
  // CTU edge x = 1024 -> device columns 63 and 64 (2 px white at A 0xE6), not 62 or 65.
  for (const int x : {63, 64}) {
    const auto c = rgb_at(first, x, 50);
    CHECK(c[0] > 220);
    CHECK(c[1] > 220);
    CHECK(c[2] > 220);
  }
  CHECK(rgb_at(first, 62, 50) == rgb_at(plain, 62, 50));
  // Tile boundary x = 1920 -> device 959..960, 2 px dashed 8 on / 4 off along y from the picture top (device -540).
  const auto on = rgb_at(first, 960, 50);  // along 590.5: 590.5 mod 12 = 2.5 -> drawn, #FF4136 opaque
  CHECK(on[0] > 230);
  CHECK(on[1] < 90);
  const auto off = rgb_at(first, 960, 56);  // along 596.5 mod 12 = 8.5 -> gap: the CTU line below shows
  CHECK(off[1] > 200);
}
