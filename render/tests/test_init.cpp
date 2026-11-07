// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.400 (chapter 10 §10.11): a VideoSurface on an OffscreenTexture target initialises a pl_gpu (lavapipe on Linux
// CI, MoltenVK on macOS), renders, and every downloaded pixel equals ViewState.background_rgba.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "ovt/render/display_caps.h"
#include "ovt/render/export.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/picture.h"
#include "ovt/render/video_surface.h"
// OverlaySet is opaque in overlay_set.h; destroying a std::unique_ptr<OverlaySet> needs the complete type (#209).
#include "overlay/overlay_set_impl.h"
#include "surface/output_download.h"

namespace {

using ovt::render::DisplayTarget;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::OutputPixels;

DisplayTarget offscreen(int w, int h, DisplayTarget::OffscreenFormat fmt = DisplayTarget::OffscreenFormat::Rgba8Srgb) {
  DisplayTarget t;
  t.kind = DisplayTarget::Kind::OffscreenTexture;
  t.width = w;
  t.height = h;
  t.offscreen_format = fmt;
  return t;
}

// init() on a host without any Vulkan device fails at the pl_gpu stage; such hosts skip (as the WI-0.015 probe does).
// Any other failure is a test failure.
std::unique_ptr<VideoSurface> init_or_skip(const DisplayTarget& t) {
  auto surface = VideoSurface::create("render-test");
  REQUIRE(surface != nullptr);
  if (!surface->init(t)) {
    const std::string err = surface->last_error();
    INFO(err);
    CHECK(err.rfind("RND-0002", 0) == 0);
    if (err.find(": pl_gpu: ") != std::string::npos || err.find(": pl_vk_inst: ") != std::string::npos) {
      SKIP("no Vulkan device available on this host: " << err);
    }
    FAIL("VideoSurface::init failed: " << err);
  }
  return surface;
}

// Number of RGBA8 texels that differ from 0xRRGGBBAA.
size_t count_rgba8_mismatches(const OutputPixels& px, uint32_t rgba) {
  const std::array<uint8_t, 4> want = {static_cast<uint8_t>(rgba >> 24U), static_cast<uint8_t>(rgba >> 16U),
                                       static_cast<uint8_t>(rgba >> 8U), static_cast<uint8_t>(rgba)};
  size_t bad = 0;
  for (size_t i = 0; i + 4 <= px.bytes.size(); i += 4) {
    if (std::memcmp(&px.bytes[i], want.data(), want.size()) != 0) {
      ++bad;
    }
  }
  return bad;
}

}  // namespace

TEST_CASE("render_test_init: offscreen surface renders background_rgba", "[fast][render][vulkan]") {
  auto surface = init_or_skip(offscreen(64, 48));
  CHECK(std::string(surface->last_error()).empty());
  CHECK(surface->active_encoding() == DisplayTarget::OutputEncoding::Srgb8);

  const auto tex = surface->output_texture();
  CHECK(tex.interop == DisplayTarget::Interop::None);
  CHECK(tex.handle != nullptr);
  CHECK(tex.width == 64);
  CHECK(tex.height == 48);
  CHECK(tex.format == DisplayTarget::OffscreenFormat::Rgba8Srgb);

  ViewState view;  // background_rgba defaults to 0x202020FF
  surface->render(view);
  OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(*surface, px));
  REQUIRE(px.width == 64);
  REQUIRE(px.height == 48);
  REQUIRE(px.texel_bytes == 4);
  REQUIRE(px.bytes.size() == size_t{64} * 48 * 4);
  CHECK(count_rgba8_mismatches(px, 0x202020FFU) == 0);

  SECTION("a new background colour replaces the old one") {
    view.background_rgba = 0x11AA44C0U;
    surface->render(view);
    REQUIRE(ovt::render::detail::download_output(*surface, px));
    CHECK(count_rgba8_mismatches(px, 0x11AA44C0U) == 0);
  }

  SECTION("resize re-creates the output texture at the new size") {
    REQUIRE(surface->resize(17, 9));
    CHECK(surface->output_texture().width == 17);
    CHECK(surface->output_texture().height == 9);
    surface->render(view);
    REQUIRE(ovt::render::detail::download_output(*surface, px));
    REQUIRE(px.bytes.size() == size_t{17} * 9 * 4);
    CHECK(count_rgba8_mismatches(px, 0x202020FFU) == 0);
    CHECK_FALSE(surface->resize(0, 9));
    CHECK(std::string(surface->last_error()).rfind("RND-0002", 0) == 0);
  }

  SECTION("the inert entry points of later work items are safe to call") {
    surface->set_overlays(nullptr);
    CHECK_FALSE(surface->pick(1, 1).hit);
    CHECK_FALSE(surface->measure().valid);
  }
}

TEST_CASE("render_test_init: Rgba16F offscreen target", "[fast][render][vulkan]") {
  auto surface = init_or_skip(offscreen(8, 8, DisplayTarget::OffscreenFormat::Rgba16F));
  ViewState view;
  view.background_rgba = 0x000000FFU;  // 0.0 and 1.0 are exact in half float
  surface->render(view);
  OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(*surface, px));
  REQUIRE(px.texel_bytes == 8);
  // IEEE half: 0.0 = 0x0000, 1.0 = 0x3C00 (little-endian storage).
  const std::array<uint8_t, 8> want = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C};
  size_t bad = 0;
  for (size_t i = 0; i + 8 <= px.bytes.size(); i += 8) {
    if (std::memcmp(&px.bytes[i], want.data(), want.size()) != 0) {
      ++bad;
    }
  }
  CHECK(bad == 0);
}

TEST_CASE("render_test_init: unsupported targets fail with RND-0002", "[fast][render]") {
  auto surface = VideoSurface::create();
  REQUIRE(surface != nullptr);
  CHECK(std::string(surface->last_error()).empty());

  DisplayTarget native = offscreen(16, 16);
  native.kind = DisplayTarget::Kind::MetalLayer;
  CHECK_FALSE(surface->init(native));
  CHECK(std::string(surface->last_error()).rfind("RND-0002 renderer init failed: target: ", 0) == 0);

  DisplayTarget interop = offscreen(16, 16);
  interop.interop = DisplayTarget::Interop::GlTexture;
  CHECK_FALSE(surface->init(interop));
  CHECK(std::string(surface->last_error()).rfind("RND-0002", 0) == 0);

  CHECK_FALSE(surface->init(offscreen(0, 16)));
  CHECK(std::string(surface->last_error()).rfind("RND-0002", 0) == 0);

  // Without a successful init() nothing is rendered and nothing can be downloaded.
  surface->render(ViewState{});
  CHECK(surface->output_texture().handle == nullptr);
  OutputPixels px;
  CHECK_FALSE(ovt::render::detail::download_output(*surface, px));
  CHECK(px.bytes.empty());
  CHECK_FALSE(surface->export_png("unused.png", ovt::render::ExportKind::SdrPng8, nullptr));
}

// The §10.2 headers are self-contained and their defaults are those of the spec.
TEST_CASE("render_test_init: public header defaults", "[fast][render]") {
  const ovt::render::Picture pic;
  CHECK(pic.num_planes == 3);
  CHECK(pic.planes[0].sample_bytes == 1);
  CHECK(pic.dovi_rpu == nullptr);

  const ovt::render::OverlayConfig cfg;
  CHECK(cfg.layers == 0);
  CHECK(cfg.selected_block == 0xFFFFFFFFU);
  CHECK(static_cast<uint32_t>(ovt::render::OverlayLayer::SelectionRect) == (1U << 17U));

  const ovt::render::DisplayCaps caps{};
  CHECK_FALSE(caps.hdr_capable);
  CHECK(sizeof(caps.platform_detail) == 128);

  const ovt::render::CompareMode cmp;
  CHECK(cmp.kind == ovt::render::CompareMode::Kind::Single);
  const ovt::render::Pick pick;
  CHECK(pick.block_id == 0xFFFFFFFFU);
  CHECK(ovt::render::ViewState{}.background_rgba == 0x202020FFU);
}
