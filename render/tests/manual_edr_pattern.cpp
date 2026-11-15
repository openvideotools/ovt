// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Manual EDR tool (chapter 10 WI-1.406 step 5, acceptance test 1): renders tests/corpora/patterns/pq_patches_1000
// (six BT.2020 PQ grey patches: 1, 100, 203, 1000, 4000, 10000 nits) into a NATIVE CAMetalLayer target and prints what
// the operator records in docs/acceptance/wi/WI-1.406.md. Not part of ctest (it needs a display).
//
//   manual_edr_pattern [--encoding scrgb|pq|hlg|srgb] [--seconds N]
//
// Pass: with HDR on an XDR display, active_encoding() == ScRgbLinear16F and the 1000-nit patch (4th from the left)
// measures >= 4x the 203-nit patch (3rd) with a colorimeter, or is visibly brighter than the white strip below.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "manual_edr_window.h"

#include "ovt/render/picture.h"
#include "ovt/render/video_surface.h"

namespace {

using ovt::render::DisplayTarget;
using Encoding = DisplayTarget::OutputEncoding;

const char* encoding_name(Encoding e) {
  constexpr std::array<const char*, 4> kNames = {"Srgb8", "ScRgbLinear16F", "Pq10", "Hlg10"};  // enum order
  return kNames.at(static_cast<size_t>(e));
}

// The full 1920x1080 10-bit 4:2:0 pattern from its expected.json: limited-range luma codes per patch column, neutral
// chroma (the same frame tools/gen_hdr_patterns.py encodes).
struct Pattern {
  ovt::render::Picture pic;
  std::vector<std::vector<uint16_t>> planes;
};

bool load_pattern(const std::string& dir, Pattern& out) {
  std::ifstream in(dir + "/pq_patches_1000.expected.json");
  if (!in.good()) {
    return false;
  }
  const nlohmann::json exp = nlohmann::json::parse(in);
  const auto w = exp.at("width").get<uint32_t>();
  const auto h = exp.at("height").get<uint32_t>();
  ovt::render::Picture& pic = out.pic;
  pic.coded_width = w;
  pic.coded_height = h;
  pic.color.struct_size = sizeof(ovt_ColorDescription);
  pic.color.bit_depth = 10;
  pic.color.chroma_format = 1;
  pic.color.primaries = 9;  // BT.2020
  pic.color.transfer = 16;  // PQ
  pic.color.matrix = 9;     // BT.2020 NCL
  pic.color.chroma_siting = 1;
  pic.num_planes = 3;
  pic.generation = 1;
  // Samples are host-endian uint16 (little-endian on every Mac), LSB-aligned; chroma is neutral (512).
  std::vector<uint16_t> row(w, 512);
  const auto& columns = exp.at("patch_columns");
  for (size_t k = 0; k < columns.size(); ++k) {
    std::fill(row.begin() + columns[k][0].get<std::ptrdiff_t>(), row.begin() + columns[k][1].get<std::ptrdiff_t>(),
              exp.at("codes_limited")[k].get<uint16_t>());
  }
  out.planes = {{}, std::vector<uint16_t>(size_t{w / 2} * (h / 2), 512), {}};
  out.planes[2] = out.planes[1];
  for (uint32_t y = 0; y < h; ++y) {
    out.planes[0].insert(out.planes[0].end(), row.begin(), row.end());
  }
  for (uint8_t i = 0; i < 3; ++i) {
    ovt::render::PlaneData& p = pic.planes[i];
    p.width = i == 0 ? w : w / 2;
    p.height = i == 0 ? h : h / 2;
    p.sample_bytes = 2;
    p.stride_bytes = p.width * 2;
    p.data =
        reinterpret_cast<const uint8_t*>(out.planes[i].data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  }
  return true;
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-pointer-arithmetic): a CLI tool
int main(int argc, char** argv) try {
  Encoding requested = Encoding::ScRgbLinear16F;
  double seconds = 60.0;
  const std::string patterns = OVT_PATTERNS_DIR;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const std::string value = argv[i + 1];
    if (key == "--encoding") {
      requested = value == "pq"     ? Encoding::Pq10
                  : value == "hlg"  ? Encoding::Hlg10
                  : value == "srgb" ? Encoding::Srgb8
                                    : Encoding::ScRgbLinear16F;
    } else if (key == "--seconds") {
      seconds = std::strtod(value.c_str(), nullptr);
    }
  }

  Pattern pattern;
  if (!load_pattern(patterns, pattern)) {
    (void)std::fprintf(stderr, "cannot read %s/pq_patches_1000.expected.json\n", patterns.c_str());
    return 2;
  }
  ovt::render::manual::EdrWindow win;
  if (!ovt::render::manual::open_edr_window(960, 540, win)) {
    (void)std::fprintf(stderr, "no window server\n");
    return 2;
  }

  DisplayTarget t;
  t.kind = DisplayTarget::Kind::MetalLayer;
  t.native = win.layer;
  t.width = win.width_px;
  t.height = win.height_px;
  t.device_pixel_ratio = win.device_pixel_ratio;
  t.hdr_capable = win.potential_headroom > 1.0F;  // §10.4.5 rule (display_probe() arrives with WI-1.407)
  t.headroom = std::max(1.0F, ovt::render::manual::current_headroom());
  t.encoding = requested;
  auto surface = ovt::render::VideoSurface::create("manual-edr");
  if (!surface->init(t)) {
    (void)std::fprintf(stderr, "init failed: %s\n", surface->last_error());
    return 1;
  }
  surface->set_frame(pattern.pic);
  ovt::render::ViewState view;
  view.zoom = static_cast<double>(win.width_px) / pattern.pic.coded_width;
  view.pan_x = pattern.pic.coded_width / 2.0;
  view.pan_y = pattern.pic.coded_height / 2.0;

  std::printf("requested encoding: %s\nactive_encoding(): %s\nlast_error(): \"%s\"\n", encoding_name(requested),
              encoding_name(surface->active_encoding()), surface->last_error());
  std::printf("potential headroom: %.3f, current headroom: %.3f, device pixel ratio: %.1f\n",
              static_cast<double>(win.potential_headroom), static_cast<double>(ovt::render::manual::current_headroom()),
              static_cast<double>(win.device_pixel_ratio));
  std::printf("patches: 1, 100, 203, 1000, 4000, 10000 nits; pass if the 4th >= 4x the 3rd or above the white strip\n");

  float last = -1.0F;
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end) {
    surface->render(view);
    ovt::render::manual::pump_events(1.0 / 60.0);
    const float now = ovt::render::manual::current_headroom();
    if (now != last) {
      std::printf("headroom now %.3f\n", static_cast<double>(now));
      last = now;
    }
  }
  return 0;
} catch (const std::exception& e) {
  (void)std::fprintf(stderr, "manual_edr_pattern: %s\n", e.what());
  return 1;
}
// NOLINTEND(cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-pointer-arithmetic)
