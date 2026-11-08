// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Helpers shared by the render tests that draw pictures (WI-1.401, WI-1.403): an owning planar Picture and an
// offscreen VideoSurface that skips on hosts without Vulkan.

#ifndef OVT_RENDER_TESTS_RENDER_TEST_SUPPORT_H
#define OVT_RENDER_TESTS_RENDER_TEST_SUPPORT_H

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif

#include "ovt/render/picture.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::test {

// A planar picture that owns its samples.
struct OwnedPicture {
  Picture pic;
  std::vector<std::vector<uint8_t>> storage;

  // chroma_format: 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4.
  OwnedPicture(uint32_t w, uint32_t h, uint8_t bit_depth, uint8_t chroma_format, uint8_t sample_bytes) {
    pic.coded_width = w;
    pic.coded_height = h;
    pic.color.struct_size = sizeof(ovt_ColorDescription);
    pic.color.bit_depth = bit_depth;
    pic.color.chroma_format = chroma_format;
    pic.color.primaries = 1;
    pic.color.transfer = 1;
    pic.color.matrix = 1;
    pic.color.full_range = 0;
    pic.color.chroma_siting = 1;
    pic.num_planes = chroma_format == 0 ? 1 : 3;
    const uint32_t sub_x = (chroma_format == 1 || chroma_format == 2) ? 2 : 1;
    const uint32_t sub_y = chroma_format == 1 ? 2 : 1;
    storage.resize(pic.num_planes);
    for (uint8_t i = 0; i < pic.num_planes; ++i) {
      ovt::render::PlaneData& p = pic.planes[i];
      p.width = i == 0 ? w : (w + sub_x - 1) / sub_x;
      p.height = i == 0 ? h : (h + sub_y - 1) / sub_y;
      p.sample_bytes = sample_bytes;
      p.stride_bytes = p.width * sample_bytes + 8;  // padded rows: the stride must be honoured
      storage[i].assign(static_cast<size_t>(p.stride_bytes) * p.height, 0xEE);
      p.data = storage[i].data();
    }
  }

  void set(uint8_t plane, uint32_t x, uint32_t y, uint32_t value) {
    const ovt::render::PlaneData& p = pic.planes[plane];
    std::vector<uint8_t>& buf = storage[plane];
    const size_t at = (static_cast<size_t>(y) * p.stride_bytes) + (static_cast<size_t>(x) * p.sample_bytes);
    if (p.sample_bytes == 1) {
      buf.at(at) = static_cast<uint8_t>(value);
    } else {
      buf.at(at) = static_cast<uint8_t>(value & 0xFFU);  // little-endian, LSB-aligned
      buf.at(at + 1) = static_cast<uint8_t>(value >> 8U);
    }
  }

  void fill_plane(uint8_t plane, uint32_t value) {
    for (uint32_t y = 0; y < pic.planes[plane].height; ++y) {
      for (uint32_t x = 0; x < pic.planes[plane].width; ++x) {
        set(plane, x, y, value);
      }
    }
  }
};

#if defined(__linux__)
// Marks every loaded Vulkan ICD (libvulkan_<driver>.so) RTLD_NODELETE. Rendering makes lavapipe JIT shaders on its own
// threads; when the loader unloads the ICD at vkDestroyInstance, LeakSanitizer can no longer see the ICD's (and
// libLLVM's) globals that still reference those allocations, and reports them as indirect leaks of "<unknown module>"
// frames that no suppression can name. Keeping the ICD mapped until exit leaves those globals visible, so LSan still
// checks everything libovt-render allocates.
inline void pin_vulkan_icds() {
  dl_iterate_phdr(
      [](struct dl_phdr_info* info, size_t /*size*/, void* /*data*/) -> int {
        const std::string name = info->dlpi_name != nullptr ? info->dlpi_name : "";
        if (name.find("libvulkan_") != std::string::npos) {
          // RTLD_NOLOAD never loads anything new; RTLD_NODELETE keeps the already loaded ICD mapped until exit.
          static_cast<void>(dlopen(name.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE));
        }
        return 0;
      },
      nullptr);
}
#else
inline void pin_vulkan_icds() {}
#endif

// A VideoSurface on an offscreen target of w x h (Rgba8Srgb unless told otherwise). Hosts without a Vulkan device skip.
inline std::unique_ptr<VideoSurface> surface_or_skip(
    int w, int h, DisplayTarget::OffscreenFormat format = DisplayTarget::OffscreenFormat::Rgba8Srgb) {
  auto surface = VideoSurface::create("render-test");
  REQUIRE(surface != nullptr);
  DisplayTarget t;
  t.width = w;
  t.height = h;
  t.offscreen_format = format;
  if (!surface->init(t)) {
    const std::string err = surface->last_error();
    if (err.find(": pl_gpu: ") != std::string::npos || err.find(": pl_vk_inst: ") != std::string::npos) {
      SKIP("no Vulkan device available on this host: " << err);
    }
    FAIL("VideoSurface::init failed: " << err);
  }
  pin_vulkan_icds();
  return surface;
}

}  // namespace ovt::render::test

#endif  // OVT_RENDER_TESTS_RENDER_TEST_SUPPORT_H
