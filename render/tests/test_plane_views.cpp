// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.408 (chapter 10 §10.11): plane modes Y / U / V / gray and raw code values (§10.6.1), with the goldens
// plane_y_10bit.png, plane_u_nearest.png and plane_gray_pq.png (tests/golden/render/, png_io.h).

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "ovt/render/video_surface.h"
#include "pipeline/plane_views.h"
#include "png_io.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::OutputPixels;
using ovt::render::test::OwnedPicture;
using ovt::render::test::RgbImage;
using ovt::render::test::surface_or_skip;

struct Rgb {
  int r = 0;
  int g = 0;
  int b = 0;
};

Rgb pixel(const OutputPixels& px, int x, int y) {
  const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(px.width)) + static_cast<size_t>(x)) * 4;
  return Rgb{px.bytes.at(i), px.bytes.at(i + 1), px.bytes.at(i + 2)};
}

bool grey(const Rgb& p) {
  return p.r == p.g && p.g == p.b;
}

// A view of the whole w x h picture at `zoom`, centred on a viewport of (w * zoom) x (h * zoom).
ViewState fit(double w, double h, int plane_mode, double zoom = 1.0) {
  ViewState view;
  view.zoom = zoom;
  view.pan_x = w / 2.0;
  view.pan_y = h / 2.0;
  view.plane_mode = plane_mode;
  return view;
}

OutputPixels render(VideoSurface& surface, const ViewState& view) {
  surface.render(view);
  REQUIRE(std::string(surface.last_error()).empty());
  OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(surface, px));
  return px;
}

// Renders twice and requires byte-identical output (§10.9.2 item 5, FR-PLN-101).
OutputPixels render_deterministic(VideoSurface& surface, const ViewState& view) {
  const OutputPixels first = render(surface, view);
  const OutputPixels second = render(surface, view);
  REQUIRE(first.bytes == second.bytes);
  return first;
}

// The RGB of an Rgba8 output.
RgbImage to_rgb(const OutputPixels& px) {
  REQUIRE(px.texel_bytes == 4);
  RgbImage img;
  img.width = px.width;
  img.height = px.height;
  for (size_t i = 0; i + 3 < px.bytes.size(); i += 4) {
    img.rgb.insert(img.rgb.end(), px.bytes.begin() + static_cast<std::ptrdiff_t>(i),
                   px.bytes.begin() + static_cast<std::ptrdiff_t>(i + 3));
  }
  return img;
}

// §10.9.2 golden rule (#465, #469): a pixel differs when any 8-bit channel is more than 2 codes off, and at most 0.1 %
// of the pixels may differ. The tests assert tighter bounds on their own pixels separately. OVT_UPDATE_GOLDEN=1
// rewrites tests/golden/render/<name> instead of comparing.
void check_golden(const std::string& name, const OutputPixels& px) {
  const RgbImage actual = to_rgb(px);
  const std::string path = std::string(OVT_RENDER_GOLDEN_DIR) + "/" + name;
  const char* update = std::getenv("OVT_UPDATE_GOLDEN");  // NOLINT(concurrency-mt-unsafe): test setup
  if (update != nullptr && std::strcmp(update, "1") == 0) {
    REQUIRE(ovt::render::test::write_png(path, actual));
    WARN("rewrote golden " << path);
    return;
  }
  const std::optional<RgbImage> read = ovt::render::test::read_png(path);
  INFO("golden " << path);
  REQUIRE(read.has_value());
  const RgbImage golden = read.value_or(RgbImage{});
  REQUIRE(golden.width == actual.width);
  REQUIRE(golden.height == actual.height);
  const size_t pixels = static_cast<size_t>(actual.width) * static_cast<size_t>(actual.height);
  size_t differing = 0;
  for (size_t p = 0; p < pixels; ++p) {
    bool differs = false;
    for (size_t c = 0; c < 3; ++c) {
      differs = differs || std::abs(int{golden.rgb.at((p * 3) + c)} - int{actual.rgb.at((p * 3) + c)}) > 2;
    }
    differing += differs ? 1 : 0;
  }
  INFO(differing << " of " << pixels << " pixels differ by more than 2 codes");
  CHECK(differing * 1000 <= pixels);
}

}  // namespace

TEST_CASE("render_test_plane_views: plane mode, neutral code and chroma rectangle helpers", "[fast][render]") {
  using ovt::render::detail::neutral_code;
  using ovt::render::detail::plane_mode;
  using ovt::render::detail::plane_rect;
  using ovt::render::detail::PlaneMode;
  ViewState view;
  for (int m = 0; m <= 4; ++m) {
    view.plane_mode = m;
    CHECK(static_cast<int>(plane_mode(view)) == m);
  }
  view.plane_mode = 5;
  CHECK(plane_mode(view) == PlaneMode::Yuv);
  view.plane_mode = -1;
  CHECK(plane_mode(view) == PlaneMode::Yuv);

  CHECK(neutral_code(8) == 128);
  CHECK(neutral_code(10) == 512);
  CHECK(neutral_code(12) == 2048);
  CHECK(neutral_code(16) == 32768);

  const pl_rect2df r = plane_rect(pl_rect2df{8.0F, 4.0F, 1928.0F, 1084.0F}, 2, 2);
  CHECK(r.x0 == 4.0F);
  CHECK(r.y0 == 2.0F);
  CHECK(r.x1 == 964.0F);
  CHECK(r.y1 == 542.0F);
  const pl_rect2df r422 = plane_rect(pl_rect2df{0.0F, 0.0F, 64.0F, 32.0F}, 2, 1);
  CHECK(r422.x1 == 32.0F);
  CHECK(r422.y1 == 32.0F);
}

// Acceptance test 1: a 10-bit frame with luma code 512, raw_code_values, plane mode Y, Rgba8Srgb -> grey 128. The
// golden is a 10-bit luma ramp over the whole code range, also in raw mode, where every output value is code >> 2.
TEST_CASE("render_test_plane_views: plane Y of a 10-bit frame shows raw code values (AT1)", "[fast][render][vulkan]") {
  constexpr uint32_t kW = 64;
  constexpr uint32_t kH = 16;
  auto surface = surface_or_skip(kW, kH);
  OwnedPicture op(kW, kH, 10, 1, 2);
  op.fill_plane(0, 512);
  op.fill_plane(1, 100);  // strongly coloured chroma: plane Y must ignore it
  op.fill_plane(2, 900);
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  ViewState view = fit(kW, kH, 1);
  view.raw_code_values = true;
  const OutputPixels flat = render(*surface, view);
  for (int y = 0; y < static_cast<int>(kH); ++y) {
    for (int x = 0; x < static_cast<int>(kW); ++x) {
      const Rgb p = pixel(flat, x, y);
      INFO("pixel (" << x << ", " << y << ") = " << p.r << "," << p.g << "," << p.b);
      REQUIRE(p.r == 128);
      REQUIRE(grey(p));
    }
  }

  // Without raw_code_values the limited range is expanded: (512 - 64) / 876 * 255 = 130.4.
  view.raw_code_values = false;
  view.dither = false;
  const Rgb expanded = pixel(render(*surface, view), 10, 5);
  CHECK(grey(expanded));
  CHECK(std::abs(expanded.r - 130) <= 1);

  // Ramp: column x holds code 16 x (0 .. 1008), row y adds y (rows differ in the two low bits only).
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      op.set(0, x, y, (16 * x) + y);
    }
  }
  op.pic.generation = 2;
  surface->set_frame(op.pic);
  view.raw_code_values = true;
  view.dither = true;  // raw code values switch dithering off by themselves
  const OutputPixels ramp = render_deterministic(*surface, view);
  for (int y = 0; y < static_cast<int>(kH); ++y) {
    for (int x = 0; x < static_cast<int>(kW); ++x) {
      const int code = (16 * x) + y;
      const Rgb p = pixel(ramp, x, y);
      INFO("code " << code << " -> " << p.r);
      REQUIRE(grey(p));
      // code / 1023 * 255 rounded; §10.6.1 writes it as code >> 2, which differs by at most one.
      REQUIRE(std::abs(p.r - (code >> 2)) <= 1);
    }
  }
  check_golden("plane_y_10bit.png", ramp);
}

// Acceptance test 2: plane U at zoom 8 on 4:2:0 -> each chroma sample covers 16 x 16 device pixels with sharp edges.
TEST_CASE("render_test_plane_views: plane U at zoom 8 is nearest-neighbour at chroma resolution (AT2)",
          "[fast][render][vulkan]") {
  constexpr uint32_t kW = 16;  // luma; chroma is 8 x 8
  constexpr uint32_t kH = 16;
  constexpr int kZoom = 8;
  constexpr int kCell = 16;  // device pixels per chroma sample: zoom x 2
  auto surface = surface_or_skip(kW * kZoom, kH * kZoom);
  OwnedPicture op(kW, kH, 8, 1, 1);
  op.fill_plane(0, 235);
  // Every chroma sample distinct, neighbours far apart: Cb = 16 + 3 * (8 cy + cx) with a checkerboard of +-, Cr flat.
  auto cb = [](int cx, int cy) {
    const int base = 16 + (3 * ((8 * cy) + cx));
    return ((cx + cy) & 1) != 0 ? 255 - base : base;
  };
  for (int cy = 0; cy < static_cast<int>(kH / 2); ++cy) {
    for (int cx = 0; cx < static_cast<int>(kW / 2); ++cx) {
      op.set(1, static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), static_cast<uint32_t>(cb(cx, cy)));
    }
  }
  op.fill_plane(2, 128);
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  ViewState view = fit(kW, kH, 2, kZoom);
  view.dither = false;
  const OutputPixels px = render_deterministic(*surface, view);
  for (int cy = 0; cy < static_cast<int>(kH / 2); ++cy) {
    for (int cx = 0; cx < static_cast<int>(kW / 2); ++cx) {
      const Rgb first = pixel(px, cx * kCell, cy * kCell);
      INFO("chroma sample (" << cx << ", " << cy << ") code " << cb(cx, cy) << " -> " << first.r);
      CHECK(grey(first));
      // Full-range normalisation: the 8-bit code is the output value (± 1 for GPU rounding).
      CHECK(std::abs(first.r - cb(cx, cy)) <= 1);
      // Sharp edges: all 16 x 16 pixels of the sample are identical.
      int mismatches = 0;
      for (int y = cy * kCell; y < (cy + 1) * kCell; ++y) {
        for (int x = cx * kCell; x < (cx + 1) * kCell; ++x) {
          const Rgb p = pixel(px, x, y);
          mismatches += (p.r != first.r || p.g != first.g || p.b != first.b) ? 1 : 0;
        }
      }
      CHECK(mismatches == 0);
    }
  }
  check_golden("plane_u_nearest.png", px);

  // Plane V of the same picture: the flat Cr plane, neutral 128 -> mid-grey everywhere.
  view.plane_mode = 3;
  const OutputPixels v = render(*surface, view);
  for (int y = 0; y < static_cast<int>(kH) * kZoom; y += 7) {
    for (int x = 0; x < static_cast<int>(kW) * kZoom; x += 7) {
      const Rgb p = pixel(v, x, y);
      REQUIRE(grey(p));
      REQUIRE(std::abs(p.r - 128) <= 1);
    }
  }
}

// Mode 4: the normal path with neutral chroma. The PQ patches with strongly coloured chroma render as the same grey
// patches (tone-mapped like the normal view) as the pattern with neutral chroma.
TEST_CASE("render_test_plane_views: gray mode is the normal path without colour (PQ patches)",
          "[fast][render][vulkan]") {
  // The six patches of tests/corpora/patterns/pq_patches_1000 (limited-range 10-bit codes), 16 columns each.
  const std::vector<uint32_t> codes = {195, 509, 573, 723, 855, 940};
  constexpr uint32_t kPatch = 16;
  const auto w = static_cast<uint32_t>(codes.size()) * kPatch;
  constexpr uint32_t kH = 8;
  auto make = [&](uint32_t cb, uint32_t cr) {
    OwnedPicture op(w, kH, 10, 1, 2);
    op.pic.color.primaries = 9;  // BT.2020
    op.pic.color.transfer = 16;  // PQ
    op.pic.color.matrix = 9;     // BT.2020 NCL
    for (uint32_t y = 0; y < kH; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        op.set(0, x, y, codes[x / kPatch]);
      }
    }
    op.fill_plane(1, cb);
    op.fill_plane(2, cr);
    return op;
  };
  auto surface = surface_or_skip(static_cast<int>(w), kH);
  ViewState view = fit(w, kH, 0);
  view.dither = false;

  OwnedPicture neutral = make(512, 512);
  neutral.pic.generation = 1;
  surface->set_frame(neutral.pic);
  REQUIRE(std::string(surface->last_error()).empty());
  const OutputPixels reference = render(*surface, view);

  OwnedPicture coloured = make(300, 800);
  coloured.pic.generation = 2;
  surface->set_frame(coloured.pic);
  const OutputPixels colour = render(*surface, view);
  view.plane_mode = 4;
  const OutputPixels gray = render_deterministic(*surface, view);

  int colour_spread = 0;
  for (uint32_t p = 0; p < codes.size(); ++p) {
    const int x = static_cast<int>((p * kPatch) + (kPatch / 2));
    const Rgb g = pixel(gray, x, kH / 2);
    const Rgb ref = pixel(reference, x, kH / 2);
    const Rgb c = pixel(colour, x, kH / 2);
    INFO("patch " << p << ": gray " << g.r << "," << g.g << "," << g.b << " reference " << ref.r << "," << ref.g << ","
                  << ref.b);
    CHECK(std::abs(g.r - ref.r) <= 1);
    CHECK(std::abs(g.g - ref.g) <= 1);
    CHECK(std::abs(g.b - ref.b) <= 1);
    colour_spread = std::max(colour_spread, std::max(std::abs(c.r - c.g), std::abs(c.b - c.g)));
  }
  CHECK(colour_spread > 20);  // the coloured picture really is coloured in mode 0
  check_golden("plane_gray_pq.png", gray);

  // Plane views of HDR content are SDR views of the code values, never tone-mapped: plane Y of the brightest patch
  // (940, the top of the limited range) is white, the 203-nit patch (573) is (573 - 64) / 876 = 58 % grey.
  view.plane_mode = 1;
  const OutputPixels y = render(*surface, view);
  CHECK(pixel(y, static_cast<int>((5 * kPatch) + 2), 2).r >= 254);
  CHECK(std::abs(pixel(y, static_cast<int>((2 * kPatch) + 2), 2).r - 148) <= 1);
}

TEST_CASE("render_test_plane_views: chroma modes of a 4:0:0 picture are neutral grey", "[fast][render][vulkan]") {
  constexpr uint32_t kW = 8;
  constexpr uint32_t kH = 8;
  auto surface = surface_or_skip(kW, kH);
  OwnedPicture op(kW, kH, 8, 0, 1);
  op.fill_plane(0, 200);
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());
  for (int mode = 2; mode <= 3; ++mode) {
    ViewState view = fit(kW, kH, mode);
    view.dither = false;
    const Rgb p = pixel(render(*surface, view), 3, 3);
    INFO("plane mode " << mode);
    CHECK(grey(p));
    CHECK(std::abs(p.r - 128) <= 1);
  }
  // Gray mode of a 4:0:0 picture is the normal view.
  ViewState normal = fit(kW, kH, 0);
  normal.dither = false;
  ViewState gray = normal;
  gray.plane_mode = 4;
  CHECK(render(*surface, normal).bytes == render(*surface, gray).bytes);
}

// Switching plane modes reuses the uploaded planes: a picture handed to set_frame() once can be shown in every mode,
// and coming back to mode 0 gives the original output.
TEST_CASE("render_test_plane_views: switching modes keeps the uploaded picture", "[fast][render][vulkan]") {
  constexpr uint32_t kW = 16;
  constexpr uint32_t kH = 8;
  auto surface = surface_or_skip(kW, kH);
  OwnedPicture op(kW, kH, 8, 1, 1);
  op.fill_plane(0, 120);
  op.fill_plane(1, 60);
  op.fill_plane(2, 200);
  op.pic.generation = 3;
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).empty());
  ViewState view = fit(kW, kH, 0);
  view.dither = false;
  const OutputPixels before = render(*surface, view);
  const std::vector<int> expected = {-1, -1, 60, 200, -1};  // modes 2 and 3 show the full-range chroma code
  for (int mode = 1; mode <= 4; ++mode) {
    view.plane_mode = mode;
    const Rgb p = pixel(render(*surface, view), 4, 4);
    INFO("plane mode " << mode);
    CHECK(grey(p));
    if (expected[static_cast<size_t>(mode)] >= 0) {
      CHECK(std::abs(p.r - expected[static_cast<size_t>(mode)]) <= 1);
    }
  }
  view.plane_mode = 0;
  CHECK(render(*surface, view).bytes == before.bytes);
}
