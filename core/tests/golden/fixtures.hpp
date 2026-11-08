// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The three hand-built FrameAnalysis fixtures of WI-0.093 (chapter 06 §6.10, §6.11). Builder code is the
// source of truth: gen_fixtures writes the checked-in files from it, test_model_roundtrip compares them.
// Every buffer is built with the object API, so it is canonical by construction (rule V-032).

#ifndef OVT_TESTS_GOLDEN_FIXTURES_HPP
#define OVT_TESTS_GOLDEN_FIXTURES_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ovt::golden {

/// A finished, identifier-carrying (`OVTA`), NOT size-prefixed FrameAnalysis buffer.
using Bytes = std::vector<std::uint8_t>;

/// One checked-in fixture: its file stem and builder.
struct FixtureSpec {
  std::string_view stem;  ///< `tiny16_hevc` -> `tiny16_hevc.fa`, `tiny16_hevc.fa.sha256`
  Bytes (*build)();
};

/// §6.10: HEVC 16×16, one I slice, four 8×8 intra CUs.
[[nodiscard]] Bytes build_tiny16_hevc();

/// 16×16 4:2:0 8-bit synthetic gradient (384 bytes), the picture `tiny16_hevc.fa` describes (YUView checks).
[[nodiscard]] Bytes build_tiny16_yuv();

/// The `.fa` fixtures in a fixed order.
inline constexpr std::array<FixtureSpec, 1> kFixtures{{
    {"tiny16_hevc", &build_tiny16_hevc},
}};

}  // namespace ovt::golden

#endif
