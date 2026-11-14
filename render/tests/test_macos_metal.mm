// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.406 on macOS: VideoSurface on a real CAMetalLayer through MoltenVK (§10.4.1). The layer is not in a window, so
// this checks the layer properties, the chosen encoding and that frames present; what the display shows is the manual
// record of acceptance test 1 (render/tests/manual_edr_pattern.cpp, docs/acceptance/wi/WI-1.406.md).

#import <QuartzCore/QuartzCore.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "ovt/render/video_surface.h"
#include "render_test_support.h"

namespace {

using ovt::render::DisplayTarget;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using Encoding = DisplayTarget::OutputEncoding;

DisplayTarget layer_target(CAMetalLayer* layer, Encoding encoding, bool hdr_capable) {
  DisplayTarget t;
  t.kind = DisplayTarget::Kind::MetalLayer;
  t.native = (__bridge void*)layer;
  t.width = 64;
  t.height = 32;
  t.device_pixel_ratio = 2.0F;
  t.hdr_capable = hdr_capable;
  t.headroom = hdr_capable ? 2.0F : 1.0F;
  t.encoding = encoding;
  return t;
}

bool layer_space_is(CAMetalLayer* layer, CFStringRef name) {
  CGColorSpaceRef space = layer.colorspace;
  if (space == nullptr) {
    return false;
  }
  CFStringRef actual = CGColorSpaceCopyName(space);
  const bool same = actual != nullptr && CFStringCompare(actual, name, 0) == kCFCompareEqualTo;
  if (actual != nullptr) {
    CFRelease(actual);
  }
  return same;
}

std::unique_ptr<VideoSurface> layer_surface_or_skip(const DisplayTarget& t) {
  ovt::render::test::install_crash_backtrace();
  auto surface = VideoSurface::create("render-test");
  if (!surface->init(t)) {
    const std::string err = surface->last_error();
    if (err.find(": pl_gpu: ") != std::string::npos || err.find(": pl_vk_inst: ") != std::string::npos) {
      SKIP("no Metal device available on this host: " << err);
    }
    FAIL("VideoSurface::init failed: " << err);
  }
  return surface;
}

}  // namespace

TEST_CASE("render_test_macos_metal: NATIVE ScRgbLinear16F on an EDR CAMetalLayer (acceptance test 1, automated part)",
          "[fast][render][vulkan]") {
  CAMetalLayer* layer = [CAMetalLayer layer];
  auto surface = layer_surface_or_skip(layer_target(layer, Encoding::ScRgbLinear16F, true));
  CHECK(surface->active_encoding() == Encoding::ScRgbLinear16F);
  CHECK(std::string(surface->last_error()).empty());
  CHECK(layer.wantsExtendedDynamicRangeContent);
  CHECK(layer.pixelFormat == MTLPixelFormatRGBA16Float);
  CHECK(layer_space_is(layer, kCGColorSpaceExtendedLinearITUR_2020));
  CHECK(layer.EDRMetadata == nil);
  CHECK(layer.drawableSize.width == 64);
  surface->render(ViewState{});
  CHECK(std::string(surface->last_error()).empty());

  // "System PQ pass-through" on the same layer: retarget() re-picks the pair and sets the HDR10 EDR metadata.
  surface->retarget(layer_target(layer, Encoding::Pq10, true));
  CHECK(surface->active_encoding() == Encoding::Pq10);
  CHECK(layer_space_is(layer, kCGColorSpaceITUR_2100_PQ));
  CHECK(layer.EDRMetadata != nil);
  surface->render(ViewState{});
  CHECK(std::string(surface->last_error()).empty());
}

TEST_CASE("render_test_macos_metal: SDR display falls back to Srgb8 (acceptance test 2)", "[fast][render][vulkan]") {
  CAMetalLayer* layer = [CAMetalLayer layer];
  auto surface = layer_surface_or_skip(layer_target(layer, Encoding::ScRgbLinear16F, false));
  CHECK(surface->active_encoding() == Encoding::Srgb8);
  CHECK(std::string(surface->last_error()).empty());
  CHECK_FALSE(layer.wantsExtendedDynamicRangeContent);
  CHECK(layer_space_is(layer, kCGColorSpaceSRGB));
  surface->render(ViewState{});
  CHECK(std::string(surface->last_error()).empty());
}

TEST_CASE("render_test_macos_metal: a NATIVE target needs a CAMetalLayer", "[fast][render]") {
  auto surface = VideoSurface::create("render-test");
  DisplayTarget t = layer_target(nil, Encoding::ScRgbLinear16F, true);
  CHECK_FALSE(surface->init(t));
  CHECK(std::string(surface->last_error()).rfind("RND-0002 renderer init failed: target: ", 0) == 0);
}
