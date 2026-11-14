// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-0.052: the platform-independent half of HdrSurfaceView's native side (android/app/src/main/cpp/hdr_pattern.h).
// The literal dataspace values are kept here only, as the guard chapter 10 §10.4.3 asks for; hdr_surface.cpp
// static_asserts the field constants against the NDK's names.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "hdr_pattern.h"

namespace {

using ovt::android_shell::OutputEncoding;
namespace shell = ovt::android_shell;
using shell::PatternRect;

// Rectangles that are empty, leave the surface or carry a code above 10 bits.
std::size_t count_outside(const std::vector<PatternRect>& rects, int width, int height) {
  std::size_t bad = 0;
  for (const auto& rect : rects) {
    const bool inside = rect.width > 0 && rect.height > 0 && rect.x >= 0 && rect.y >= 0 &&
                        rect.x + rect.width <= width && rect.y + rect.height <= height;
    const bool codes_ok = rect.code[0] <= 1023 && rect.code[1] <= 1023 && rect.code[2] <= 1023;
    bad += (inside && codes_ok) ? 0U : 1U;
  }
  return bad;
}

// The kPatchNits values without a grey patch (wider than a label pixel) of their PQ code.
std::vector<int> missing_patches(const std::vector<PatternRect>& rects) {
  std::vector<int> missing;
  for (const int nits : shell::kPatchNits) {
    const std::uint16_t code = shell::code10(OutputEncoding::Pq10, nits);
    const std::array<std::uint16_t, 3> grey{code, code, code};
    if (std::none_of(rects.begin(), rects.end(),
                     [&](const PatternRect& rect) { return rect.width > 16 && rect.code == grey; })) {
      missing.push_back(nits);
    }
  }
  return missing;
}

bool same(const std::vector<PatternRect>& a, const std::vector<PatternRect>& b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const PatternRect& l, const PatternRect& r) {
    return l.x == r.x && l.y == r.y && l.width == r.width && l.height == r.height && l.code == r.code;
  });
}

}  // namespace

TEST_CASE("android_hdr: dataspace values are the NDK's", "[fast][android][hdr]") {
  // <android/data_space.h>: ADATASPACE_BT2020_PQ, ADATASPACE_BT2020_HLG (full range) and ADATASPACE_BT709.
  CHECK(shell::dataspace_for(OutputEncoding::Pq10) == 163971072);
  CHECK(shell::dataspace_for(OutputEncoding::Hlg10) == 168165376);
  CHECK(shell::dataspace_for(OutputEncoding::Srgb8) == 281083904);
}

TEST_CASE("android_hdr: logcat names", "[fast][android][hdr]") {
  CHECK(std::string(shell::dataspace_log_name(OutputEncoding::Pq10)) == "BT2020_PQ");
  CHECK(std::string(shell::dataspace_log_name(OutputEncoding::Hlg10)) == "BT2020_HLG");
  CHECK(std::string(shell::dataspace_log_name(OutputEncoding::Srgb8)) == "SRGB");
}

TEST_CASE("android_hdr: encoding rule of chapter 10 §10.4.3", "[fast][android][hdr]") {
  const std::vector<std::int32_t> none;
  const std::vector<std::int32_t> hdr10{shell::kHdrTypeHdr10};
  const std::vector<std::int32_t> hdr10_plus{shell::kHdrTypeHdr10Plus};
  const std::vector<std::int32_t> hlg{shell::kHdrTypeHlg};
  const std::vector<std::int32_t> all{shell::kHdrTypeDolbyVision, shell::kHdrTypeHlg, shell::kHdrTypeHdr10};
  const std::vector<std::int32_t> dv_only{shell::kHdrTypeDolbyVision};
  CHECK(shell::choose_encoding(none) == OutputEncoding::Srgb8);  // Pixel Tablet, emulator (WI-0.052 test 2)
  CHECK(shell::choose_encoding(hdr10) == OutputEncoding::Pq10);
  CHECK(shell::choose_encoding(hdr10_plus) == OutputEncoding::Pq10);
  CHECK(shell::choose_encoding(hlg) == OutputEncoding::Hlg10);
  CHECK(shell::choose_encoding(all) == OutputEncoding::Pq10);
  CHECK(shell::choose_encoding(dv_only) == OutputEncoding::Srgb8);
}

TEST_CASE("android_hdr: PQ codes of chapter 10 §10.4.4", "[fast][android][hdr]") {
  // Full-range 10-bit codes for 1, 100, 203, 1000, 4000 and 10000 cd/m².
  constexpr std::array<double, 6> kNits{1, 100, 203, 1000, 4000, 10000};
  constexpr std::array<int, 6> kCodes{153, 520, 594, 769, 923, 1023};
  for (std::size_t i = 0; i < kNits.size(); ++i) {
    INFO(kNits.at(i) << " cd/m2");
    CHECK(shell::code10(OutputEncoding::Pq10, kNits.at(i)) == kCodes.at(i));
  }
  CHECK(shell::code10(OutputEncoding::Pq10, 0.0) == 0);
}

TEST_CASE("android_hdr: HLG and SDR signals", "[fast][android][hdr]") {
  // BT.2408: 203 cd/m² reference white is HLG signal 0.75 on the 1000 cd/m² display.
  CHECK_THAT(shell::hlg_signal(203.0), Catch::Matchers::WithinAbs(0.75, 0.002));
  CHECK_THAT(shell::hlg_signal(1000.0), Catch::Matchers::WithinAbs(1.0, 1e-6));
  CHECK(shell::code10(OutputEncoding::Hlg10, 4000.0) == 1023);
  // SDR: 203 cd/m² is white, brighter clips.
  CHECK(shell::code10(OutputEncoding::Srgb8, 203.0) == 1023);
  CHECK(shell::code10(OutputEncoding::Srgb8, 10000.0) == 1023);
  CHECK(shell::code10(OutputEncoding::Srgb8, 100.0) < 1023);
}

TEST_CASE("android_hdr: test pattern layout", "[fast][android][hdr]") {
  CHECK(shell::build_test_pattern(32, 32, OutputEncoding::Pq10).empty());

  const auto [width, height] = GENERATE(table<int, int>({{1920, 1080}, {800, 600}, {2560, 1600}}));
  INFO(width << "x" << height);
  const auto rects = shell::build_test_pattern(width, height, OutputEncoding::Pq10);
  REQUIRE(rects.size() > static_cast<std::size_t>(shell::kRampSteps));
  CHECK(count_outside(rects, width, height) == 0);
  // The ramp: kRampSteps rectangles from code 0 to 1023.
  CHECK(rects.front().code[0] == 0);
  CHECK(rects.at(shell::kRampSteps - 1).code[0] == 1023);
  // Every labelled patch is present with its PQ code, and 1000 cd/m² is above 203 cd/m² (WI-0.052 test 1).
  CHECK(missing_patches(rects).empty());
  CHECK(shell::code10(OutputEncoding::Pq10, 1000) > shell::code10(OutputEncoding::Pq10, 203));
  // The last rectangle is the blue bar: BT.2020 blue at 203 cd/m².
  CHECK(rects.back().code == std::array<std::uint16_t, 3>{0, 0, 594});
}

TEST_CASE("android_hdr: the pattern is deterministic", "[fast][android][hdr]") {
  for (const auto encoding : {OutputEncoding::Pq10, OutputEncoding::Hlg10, OutputEncoding::Srgb8}) {
    CHECK(same(shell::build_test_pattern(1280, 720, encoding), shell::build_test_pattern(1280, 720, encoding)));
  }
}
