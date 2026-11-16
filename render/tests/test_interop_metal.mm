// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.404 acceptance test 1 (chapter 10 §10.11): Interop::MetalTexture on macOS. The test plays the host (Qt RHI's
// Metal backend, §10.2.2): it takes the exported id<MTLTexture>, blits it into a shared MTLBuffer on its own Metal
// command queue and compares the bytes with libplacebo's download of the same texture. Objective-C++ with ARC.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ovt/render/video_surface.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::DisplayTarget;
using ovt::render::ExternalTexture;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::OutputPixels;
using ovt::render::test::OwnedPicture;
using ovt::render::test::rgb_ramp;

// Copies `tex` (BGRA/RGBA 8-bit, w x h) into host memory with a blit on a command queue of `device`.
std::vector<uint8_t> read_texture(id<MTLDevice> device, id<MTLTexture> tex, NSUInteger w, NSUInteger h) {
  const NSUInteger row = w * 4;
  const NSUInteger size = row * h;
  std::vector<uint8_t> out;
  @autoreleasepool {
    id<MTLCommandQueue> const queue = [device newCommandQueue];
    REQUIRE(static_cast<bool>(queue != nil));  // ObjC pointers compared as bool (no Catch2 StringMaker)
    id<MTLBuffer> const buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    REQUIRE(static_cast<bool>(buffer != nil));
    id<MTLCommandBuffer> const cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> const blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:row
        destinationBytesPerImage:size];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    REQUIRE(cb.status == MTLCommandBufferStatusCompleted);
    const auto* bytes = static_cast<const uint8_t*>(buffer.contents);
    out.assign(bytes, bytes + size);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return out;
}

}  // namespace

TEST_CASE("render_test_interop: MetalTexture exports an MTLTexture the host can read",
          "[fast][render][vulkan][interop]") {
  ovt::render::test::install_crash_backtrace();
  auto surface = VideoSurface::create("render-interop-metal");
  REQUIRE(surface != nullptr);
  constexpr int kW = 32;
  constexpr int kH = 16;
  DisplayTarget t;
  t.width = kW;
  t.height = kH;
  t.interop = DisplayTarget::Interop::MetalTexture;
  if (!surface->init(t)) {
    const std::string err = surface->last_error();
    if (err.find(": pl_gpu: ") != std::string::npos || err.find(": pl_vk_inst: ") != std::string::npos) {
      SKIP("no Vulkan device available on this host: " << err);
    }
    FAIL("VideoSurface::init failed: " << err);
  }

  const OwnedPicture ramp = rgb_ramp(kW, kH);
  surface->set_frame(ramp.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  @autoreleasepool {
    id<MTLDevice> const device = MTLCreateSystemDefaultDevice();  // the host's Metal device
    REQUIRE(static_cast<bool>(device != nil));
    for (int frame = 0; frame < 2; ++frame) {
      INFO("frame " << frame);
      ViewState view;
      view.background_rgba = 0xFF00FFFFU;  // any pixel left at the background would show as magenta
      view.pan_x = kW / 2.0;               // picture centred: it covers the whole target at zoom 1
      view.pan_y = kH / 2.0;
      surface->render(view);
      const ExternalTexture tex = surface->output_texture();
      REQUIRE(tex.interop == DisplayTarget::Interop::MetalTexture);
      REQUIRE(tex.handle != nullptr);
      CHECK(tex.handle2 == nullptr);
      CHECK(tex.width == kW);
      CHECK(tex.height == kH);
      CHECK(tex.layout == 0U);

      id<MTLTexture> const mtl = (__bridge id<MTLTexture>)tex.handle;
      CHECK(mtl.width == static_cast<NSUInteger>(kW));
      CHECK(mtl.height == static_cast<NSUInteger>(kH));
      CHECK(mtl.pixelFormat == MTLPixelFormatRGBA8Unorm);
      // MoltenVK renders on the system default device; the host's device is the same GPU.
      REQUIRE(mtl.device.registryID == device.registryID);

      const std::vector<uint8_t> host_bytes = read_texture(mtl.device, mtl, mtl.width, mtl.height);
      OutputPixels px;
      REQUIRE(ovt::render::detail::download_output(*surface, px));
      REQUIRE(px.texel_bytes == 4);
      CHECK(host_bytes == px.bytes);
      size_t magenta = 0;
      for (size_t i = 0; i + 3 < host_bytes.size(); i += 4) {
        if (host_bytes[i] == 0xFF && host_bytes[i + 1] == 0x00 && host_bytes[i + 2] == 0xFF) {
          ++magenta;
        }
      }
      CHECK(magenta == 0);
    }
  }

  // A resize makes a new exportable image with its own MTLTexture.
  REQUIRE(surface->resize(kW * 2, kH));
  surface->render(ViewState{});
  const ExternalTexture resized = surface->output_texture();
  REQUIRE(resized.handle != nullptr);
  @autoreleasepool {
    id<MTLTexture> const mtl = (__bridge id<MTLTexture>)resized.handle;
    CHECK(mtl.width == static_cast<NSUInteger>(kW * 2));
    const std::vector<uint8_t> host_bytes = read_texture(mtl.device, mtl, mtl.width, mtl.height);
    OutputPixels px;
    REQUIRE(ovt::render::detail::download_output(*surface, px));
    CHECK(host_bytes == px.bytes);
  }
}
