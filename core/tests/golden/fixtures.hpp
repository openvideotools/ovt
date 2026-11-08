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
/// H.264 32×32, four macroblocks (I_16x16, P_L0_16x16, P_8x8 with four sub_mb_types, P_Skip).
[[nodiscard]] Bytes build_tiny32_h264();
/// AV1 64×64, one superblock split into four 32×32 blocks (intra, NEWMV, GLOBALMV, compound).
[[nodiscard]] Bytes build_tiny64_av1();

/// 16×16 4:2:0 8-bit synthetic gradient (384 bytes), the picture `tiny16_hevc.fa` describes (YUView checks).
[[nodiscard]] Bytes build_tiny16_yuv();

/// §6.8.3 export of `tiny64_av1.fa`, written from the §6.8.3 rules by an independent reference writer
/// (WI-0.090 compares its exporter against this file). @p av1 is a `tiny64_av1.fa` buffer.
[[nodiscard]] std::string build_tiny64_av1_inspect_json(const Bytes& av1);

/// The `.fa` fixtures in a fixed order.
inline constexpr std::array<FixtureSpec, 3> kFixtures{{
    {"tiny16_hevc", &build_tiny16_hevc},
    {"tiny32_h264", &build_tiny32_h264},
    {"tiny64_av1", &build_tiny64_av1},
}};

}  // namespace ovt::golden

#endif
