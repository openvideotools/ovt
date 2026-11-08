// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.401 (chapter 10 §10.11): VideoSurface::set_frame() uploads 8/10/12/16-bit planar pictures and fills the pl_frame
// per §10.3.1 (bit depth, chroma siting, crop, generation skip, R-10.2.1 rejection). Rendered 1:1 to an Rgba8Srgb
// offscreen target on lavapipe (Linux) or MoltenVK (macOS) and read back.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif

#include "ovt/render/picture.h"
#include "ovt/render/video_surface.h"
#include "surface/output_download.h"

namespace {

using ovt::render::DisplayTarget;
using ovt::render::Picture;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::OutputPixels;

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

struct Rgb {
  int r, g, b;
};

Rgb pixel(const OutputPixels& px, int x, int y) {
  const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(px.width)) + static_cast<size_t>(x)) * 4;
  return {px.bytes[i], px.bytes[i + 1], px.bytes[i + 2]};
}

int spread(const Rgb& c) {
  const int hi = std::max({c.r, c.g, c.b});
  const int lo = std::min({c.r, c.g, c.b});
  return hi - lo;
}

#if defined(__linux__)
// Marks every loaded Vulkan ICD (libvulkan_<driver>.so) RTLD_NODELETE. Rendering makes lavapipe JIT shaders on its own
// threads; when the loader unloads the ICD at vkDestroyInstance, LeakSanitizer can no longer see the ICD's (and
// libLLVM's) globals that still reference those allocations, and reports them as indirect leaks of "<unknown module>"
// frames that no suppression can name. Keeping the ICD mapped until exit leaves those globals visible, so LSan still
// checks everything libovt-render allocates.
void pin_vulkan_icds() {
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
void pin_vulkan_icds() {}
#endif

std::unique_ptr<VideoSurface> surface_or_skip(int w, int h) {
  auto surface = VideoSurface::create("plane-fill-test");
  REQUIRE(surface != nullptr);
  DisplayTarget t;
  t.width = w;
  t.height = h;
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

// Renders slot A 1:1 (pan at the centre of the w x h source rectangle) and downloads the result.
OutputPixels render_1to1(VideoSurface& surface, double src_w, double src_h, double x0 = 0, double y0 = 0) {
  ViewState view;
  view.zoom = 1.0;
  view.pan_x = x0 + (src_w / 2.0);
  view.pan_y = y0 + (src_h / 2.0);
  view.background_rgba = 0xFF00FFFFU;  // magenta: any pixel not covered by the picture is obvious
  surface.render(view);
  OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(surface, px));
  return px;
}

// Picture whose left half is limited-range black, right half limited-range white, chroma neutral.
OwnedPicture black_white(uint32_t w, uint32_t h, uint8_t bd, uint8_t cf, uint8_t sample_bytes) {
  OwnedPicture op(w, h, bd, cf, sample_bytes);
  const uint32_t shift = bd - 8U;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      op.set(0, x, y, (x < w / 2 ? 16U : 235U) << shift);
    }
  }
  if (op.pic.num_planes == 3) {
    op.fill_plane(1, 128U << shift);
    op.fill_plane(2, 128U << shift);
  }
  return op;
}

}  // namespace

// Acceptance test 1.
TEST_CASE("render_test_plane_fill: 10-bit 4:2:0 limited ramp maps 64 to 0 and 940 to 255", "[fast][render][vulkan]") {
  constexpr uint32_t kW = 64;
  constexpr uint32_t kH = 8;
  auto surface = surface_or_skip(kW, kH);
  OwnedPicture op(kW, kH, 10, 1, 2);
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      op.set(0, x, y, 64U + ((940U - 64U) * x / (kW - 1)));
    }
  }
  op.fill_plane(1, 512);
  op.fill_plane(2, 512);
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  const OutputPixels px = render_1to1(*surface, kW, kH);
  for (int y = 0; y < static_cast<int>(kH); ++y) {
    const Rgb black = pixel(px, 0, y);
    const Rgb white = pixel(px, kW - 1, y);
    INFO("row " << y << ": black " << black.r << "," << black.g << "," << black.b << " white " << white.r << ","
                << white.g << "," << white.b);
    CHECK(black.r <= 1);
    CHECK(black.g <= 1);
    CHECK(black.b <= 1);
    CHECK(white.r >= 254);
    CHECK(white.g >= 254);
    CHECK(white.b >= 254);
  }
  // The ramp increases monotonically and stays grey: no pre-shift, no chroma cast.
  for (int x = 1; x < static_cast<int>(kW); ++x) {
    CHECK(pixel(px, x, 3).g >= pixel(px, x - 1, 3).g);
    CHECK(spread(pixel(px, x, 3)) <= 2);
  }
}

// The format matrix of step 5: every supported depth, sample size and chroma format.
TEST_CASE("render_test_plane_fill: format matrix renders limited black and white", "[fast][render][vulkan]") {
  struct Format {
    uint8_t bit_depth;
    uint8_t chroma_format;
    uint8_t sample_bytes;
  };
  const std::array<Format, 13> formats = {{{8, 1, 1},
                                           {8, 2, 1},
                                           {8, 3, 1},
                                           {8, 0, 1},
                                           {8, 1, 2},
                                           {10, 1, 2},
                                           {10, 2, 2},
                                           {10, 3, 2},
                                           {12, 1, 2},
                                           {12, 3, 2},
                                           {16, 1, 2},
                                           {16, 3, 2},
                                           {10, 0, 2}}};
  constexpr uint32_t kW = 32;
  constexpr uint32_t kH = 8;
  auto surface = surface_or_skip(kW, kH);
  uint64_t generation = 0;
  for (const Format& f : formats) {
    DYNAMIC_SECTION("bit_depth " << int{f.bit_depth} << " chroma_format " << int{f.chroma_format} << " sample_bytes "
                                 << int{f.sample_bytes}) {
      OwnedPicture op = black_white(kW, kH, f.bit_depth, f.chroma_format, f.sample_bytes);
      op.pic.generation = ++generation;
      surface->set_frame(op.pic);
      REQUIRE(std::string(surface->last_error()).empty());
      const OutputPixels px = render_1to1(*surface, kW, kH);
      for (int y = 0; y < static_cast<int>(kH); ++y) {
        const Rgb black = pixel(px, 2, y);
        const Rgb white = pixel(px, kW - 3, y);
        CHECK(std::max({black.r, black.g, black.b}) <= 1);
        CHECK(std::min({white.r, white.g, white.b}) >= 254);
      }
    }
  }
}

// Regression test for the teardown race behind the intermittent SIGSEGV of the format matrix on MoltenVK: a surface
// destroyed right after upload, render and download must not free the Vulkan device while the driver still finishes
// that work (VideoSurfaceImpl::wait_device_idle). Many short-lived surfaces make the race window recur.
TEST_CASE("render_test_plane_fill: surfaces torn down right after a download stay stable", "[fast][render][vulkan]") {
  constexpr uint32_t kW = 16;
  constexpr uint32_t kH = 4;
  constexpr int kCycles = 24;
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    INFO("cycle " << cycle);
    auto surface = surface_or_skip(kW, kH);
    OwnedPicture op = black_white(kW, kH, 10, 1, 2);
    op.pic.generation = 1;
    surface->set_frame(op.pic);
    REQUIRE(std::string(surface->last_error()).empty());
    const OutputPixels px = render_1to1(*surface, kW, kH);
    REQUIRE(std::max({pixel(px, 1, 1).r, pixel(px, 1, 1).g, pixel(px, 1, 1).b}) <= 1);
    REQUIRE(std::min({pixel(px, kW - 2, 1).r, pixel(px, kW - 2, 1).g, pixel(px, kW - 2, 1).b}) >= 254);
  }
}

TEST_CASE("render_test_plane_fill: 4:4:4 8-bit full-range RGB (matrix 0) downloads identically",
          "[fast][render][vulkan]") {
  constexpr uint32_t kW = 64;
  constexpr uint32_t kH = 4;
  auto surface = surface_or_skip(kW, kH);
  OwnedPicture op(kW, kH, 8, 3, 1);
  op.pic.color.matrix = 0;     // identity: planes are G, B, R (H.273)
  op.pic.color.transfer = 13;  // sRGB in, sRGB out
  op.pic.color.full_range = 1;
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      op.set(0, x, y, x * 4);                     // G ramp
      op.set(1, x, y, 255 - (x * 4));             // B falling ramp
      op.set(2, x, y, (x * 37 + y * 50) & 0xFF);  // R scrambled
    }
  }
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());
  const OutputPixels px = render_1to1(*surface, kW, kH);
  size_t mismatches = 0;
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      const Rgb c = pixel(px, static_cast<int>(x), static_cast<int>(y));
      const int want_r = static_cast<int>((x * 37 + y * 50) & 0xFF);
      const int want_g = static_cast<int>(x * 4);
      const int want_b = static_cast<int>(255 - (x * 4));
      if (c.r != want_r || c.g != want_g || c.b != want_b) {
        ++mismatches;
        UNSCOPED_INFO("(" << x << "," << y << ") got " << c.r << "," << c.g << "," << c.b << " want " << want_r << ","
                          << want_g << "," << want_b);
      }
    }
  }
  CHECK(mismatches == 0);
}

TEST_CASE("render_test_plane_fill: chroma siting sets the chroma plane shift", "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 16);
  struct Case {
    uint8_t siting;
    float sx;
    float sy;
  };
  // pl_chroma_location_offset: left (-0.5, 0), center (0, 0), top-left (-0.5, -0.5); 0 unknown -> left (§10.3.1).
  const std::array<Case, 4> cases = {{{0, -0.5F, 0.0F}, {1, -0.5F, 0.0F}, {2, 0.0F, 0.0F}, {3, -0.5F, -0.5F}}};
  uint64_t generation = 0;
  for (const Case& c : cases) {
    OwnedPicture op = black_white(16, 16, 10, 1, 2);
    op.pic.color.chroma_siting = c.siting;
    op.pic.generation = ++generation;
    surface->set_frame(op.pic);
    REQUIRE(std::string(surface->last_error()).empty());
    float sx = 99.0F;
    float sy = 99.0F;
    REQUIRE(ovt::render::detail::slot_a_chroma_shift(*surface, sx, sy));
    INFO("siting " << int{c.siting});
    CHECK(sx == c.sx);
    CHECK(sy == c.sy);
  }
}

// Acceptance test 2: syn_pat_siting_checker frame 0, rendered with siting 1 (left) and 2 (center). The planes are
// generated from the pattern's own description (tests/corpora/patterns/siting_checker.expected.json, WI-1.413): flat
// luma and a one-chroma-sample Cb/Cr checkerboard. With bilinear chroma upsampling, left siting puts every odd luma
// column exactly halfway between two chroma samples of opposite colour (neutral grey) and every even column on a
// chroma sample; center siting moves chroma by 0.5 luma sample, so every column sits at a 1/4-3/4 mix and none is
// neutral. That half-sample shift is what the golden comparison checks.
TEST_CASE("render_test_plane_fill: siting_checker shifts chroma by half a luma sample", "[fast][render][vulkan]") {
  std::ifstream in(std::string(OVT_PATTERNS_DIR) + "/siting_checker.expected.json");
  REQUIRE(in.good());
  const nlohmann::json exp = nlohmann::json::parse(in);
  const auto w = exp.at("width").get<uint32_t>();
  const auto h = exp.at("height").get<uint32_t>();
  REQUIRE(exp.at("bit_depth").get<int>() == 10);
  REQUIRE(exp.at("range").get<std::string>() == "limited");
  const auto luma = exp.at("luma_code").get<uint32_t>();
  const auto hi = exp.at("chroma_values").at("high").get<uint32_t>();
  const auto lo = exp.at("chroma_values").at("low").get<uint32_t>();

  OwnedPicture op(w, h, 10, 1, 2);
  op.fill_plane(0, luma);
  for (uint32_t cy = 0; cy < h / 2; ++cy) {
    for (uint32_t cx = 0; cx < w / 2; ++cx) {
      const bool odd = ((cx + cy) & 1U) != 0;  // chroma_rule of the expected JSON
      op.set(1, cx, cy, odd ? hi : lo);
      op.set(2, cx, cy, odd ? lo : hi);
    }
  }

  auto surface = surface_or_skip(static_cast<int>(w), static_cast<int>(h));
  // An interior window, away from the picture edges where the upsampler clamps.
  const int x_begin = 16;
  const int x_end = 112;
  const int y_begin = 16;
  const int y_end = 48;

  op.pic.color.chroma_siting = 1;  // left, as signalled in the stream's VUI
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());
  const OutputPixels left = render_1to1(*surface, w, h);

  op.pic.color.chroma_siting = 2;  // center
  op.pic.generation = 2;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());
  const OutputPixels center = render_1to1(*surface, w, h);

  int left_odd_max = 0;     // largest colour spread on odd columns, left siting: neutral
  int left_even_min = 255;  // smallest spread on even columns, left siting: full chroma
  int center_min = 255;     // smallest spread anywhere, center siting: never neutral
  int center_max = 0;
  for (int y = y_begin; y < y_end; ++y) {
    for (int x = x_begin; x < x_end; ++x) {
      const int sl = spread(pixel(left, x, y));
      const int sc = spread(pixel(center, x, y));
      if ((x & 1) != 0) {
        left_odd_max = std::max(left_odd_max, sl);
      } else {
        left_even_min = std::min(left_even_min, sl);
      }
      center_min = std::min(center_min, sc);
      center_max = std::max(center_max, sc);
    }
  }
  INFO("left odd max spread " << left_odd_max << ", left even min " << left_even_min << ", center " << center_min
                              << ".." << center_max);
  CHECK(left_odd_max <= 3);
  CHECK(left_even_min >= 40);
  CHECK(center_min >= 20);
  // Center samples sit between the two left-siting states: weaker than a full chroma sample, stronger than neutral.
  CHECK(center_max < left_even_min + 3);
}

TEST_CASE("render_test_plane_fill: same generation skips the re-upload", "[fast][render][vulkan]") {
  constexpr uint32_t kW = 16;
  constexpr uint32_t kH = 4;
  auto surface = surface_or_skip(kW, kH);
  OwnedPicture op(kW, kH, 8, 1, 1);
  op.fill_plane(0, 16);
  op.fill_plane(1, 128);
  op.fill_plane(2, 128);
  op.pic.generation = 7;
  surface->set_frame(op.pic);
  CHECK(pixel(render_1to1(*surface, kW, kH), 5, 1).g <= 1);

  op.fill_plane(0, 235);  // new content, same generation: still the old picture
  surface->set_frame(op.pic);
  CHECK(pixel(render_1to1(*surface, kW, kH), 5, 1).g <= 1);

  op.pic.generation = 8;
  surface->set_frame(op.pic);
  CHECK(pixel(render_1to1(*surface, kW, kH), 5, 1).g >= 254);
}

TEST_CASE("render_test_plane_fill: crop selects the conformance window", "[fast][render][vulkan]") {
  constexpr uint32_t kW = 32;
  constexpr uint32_t kH = 8;
  auto surface = surface_or_skip(16, kH);
  OwnedPicture op = black_white(kW, kH, 8, 1, 1);
  op.pic.crop = ovt::fb::Rect(8, 0, 16, static_cast<int>(kH));
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());
  // show_cropped (default): the 16-wide window x = 8..24 fills the 16-wide target, black then white.
  const OutputPixels px = render_1to1(*surface, 16, kH, 8, 0);
  for (int x = 0; x < 16; ++x) {
    const Rgb c = pixel(px, x, 3);
    INFO("x " << x);
    if (x < 8) {
      CHECK(std::max({c.r, c.g, c.b}) <= 1);
    } else {
      CHECK(std::min({c.r, c.g, c.b}) >= 254);
    }
  }
}

TEST_CASE("render_test_plane_fill: invalid pictures are rejected with RND-0001", "[fast][render][vulkan]") {
  auto surface = surface_or_skip(16, 4);
  auto expect_reject = [&](const Picture& pic, const std::string& prefix) {
    surface->set_frame(pic);
    const std::string err = surface->last_error();
    INFO(err);
    CHECK(err.rfind(prefix, 0) == 0);
    // Nothing is drawn: the whole target keeps the background colour.
    const OutputPixels px = render_1to1(*surface, 16, 4);
    const Rgb c = pixel(px, 8, 2);
    CHECK(c.r == 255);
    CHECK(c.g == 0);
    CHECK(c.b == 255);
  };

  {
    OwnedPicture op = black_white(16, 4, 10, 1, 2);
    op.pic.color.bit_depth = 17;
    expect_reject(op.pic, "RND-0001 picture rejected: bit_depth 17 / sample_bytes 2 unsupported");
  }
  {
    OwnedPicture op = black_white(16, 4, 8, 1, 1);
    for (auto& p : op.pic.planes) {
      p.sample_bytes = 3;
    }
    expect_reject(op.pic, "RND-0001 picture rejected: bit_depth 8 / sample_bytes 3 unsupported");
  }
  {
    OwnedPicture op = black_white(16, 4, 8, 1, 1);
    op.pic.color.bit_depth = 10;  // 10-bit samples cannot live in one byte
    expect_reject(op.pic, "RND-0001 picture rejected: bit_depth 10 / sample_bytes 1 unsupported");
  }
  {
    OwnedPicture op = black_white(16, 4, 8, 1, 1);
    op.pic.planes[1].width = 16;  // a 4:2:0 chroma plane must be 8 wide
    expect_reject(op.pic, "RND-0001 picture rejected: plane 1");
  }
  {
    OwnedPicture op = black_white(16, 4, 8, 1, 1);
    op.pic.planes[2].data = nullptr;
    expect_reject(op.pic, "RND-0001 picture rejected: plane 2 has no data");
  }
  {
    OwnedPicture op = black_white(16, 4, 8, 1, 1);
    op.pic.planes[0].stride_bytes = 15;
    expect_reject(op.pic, "RND-0001 picture rejected: plane 0 stride");
  }
  {
    OwnedPicture op = black_white(16, 4, 8, 1, 1);
    op.pic.num_planes = 1;  // 4:2:0 needs three planes
    expect_reject(op.pic, "RND-0001 picture rejected: num_planes");
  }
  {
    OwnedPicture op = black_white(16, 4, 8, 1, 1);
    op.pic.crop = ovt::fb::Rect(8, 0, 16, 4);
    expect_reject(op.pic, "RND-0001 picture rejected: crop");
  }
}
