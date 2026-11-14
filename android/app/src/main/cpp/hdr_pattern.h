// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Platform-independent half of the Android HDR output of WI-0.052 (chapter 13 §13.4.4 item 2, chapter 10 §10.4.3):
// the encoding rule, the dataspace each encoding is tagged with, the transfer functions and the PQ test pattern as a
// list of solid rectangles. hdr_surface.cpp draws the rectangles with GLES scissored clears; the host test
// android/app/src/test/cpp/test_hdr_pattern.cpp checks this file without the NDK.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ovt::android_shell {

// The output encodings of DisplayTarget::OutputEncoding (chapter 10 §10.2.2) that Android uses (§10.4.3).
enum class OutputEncoding : std::uint8_t { Srgb8, Pq10, Hlg10 };

// Display.HdrCapabilities.HDR_TYPE_* (also Display.Mode.getSupportedHdrTypes() on API 34+).
inline constexpr std::int32_t kHdrTypeDolbyVision = 1;
inline constexpr std::int32_t kHdrTypeHdr10 = 2;
inline constexpr std::int32_t kHdrTypeHlg = 3;
inline constexpr std::int32_t kHdrTypeHdr10Plus = 4;

// ADataSpace field constants of <android/data_space.h>. Only fields are combined, never two pre-combined
// ADATASPACE_* values (§13.4.4 item 2). hdr_surface.cpp static_asserts each against the NDK name.
inline constexpr std::int32_t kStandardBt709 = 1 << 16;
inline constexpr std::int32_t kStandardBt2020 = 6 << 16;
inline constexpr std::int32_t kTransferSmpte170m = 3 << 22;
inline constexpr std::int32_t kTransferSt2084 = 7 << 22;
inline constexpr std::int32_t kTransferHlg = 8 << 22;
inline constexpr std::int32_t kRangeFull = 1 << 27;
inline constexpr std::int32_t kRangeLimited = 2 << 27;

// §10.4.3 encoding rule: Pq10 if HDR10 or HDR10+ is supported, else Hlg10 if HLG, else Srgb8. An SDR panel (the
// Pixel Tablet, the emulator) reports no types, so it gets Srgb8.
OutputEncoding choose_encoding(std::span<const std::int32_t> hdr_types) noexcept;

// The dataspace a surface carrying `encoding` is tagged with: ADATASPACE_BT2020_PQ, ADATASPACE_BT2020_HLG (both full
// range by definition) or, for SDR, ADATASPACE_BT709 (chapter 4 FR-AND-002; SPEC-ASSUMPTION(#436): not the
// ADATASPACE_UNKNOWN of the chapter 10 §10.4.3 snippet).
std::int32_t dataspace_for(OutputEncoding encoding) noexcept;

// The <name> of the normative logcat line `OVT_HDR dataspace=<name>` (§13.4.4): BT2020_PQ, BT2020_HLG or SRGB.
const char* dataspace_log_name(OutputEncoding encoding) noexcept;

// SMPTE ST 2084 inverse EOTF: absolute luminance (cd/m², clamped to [0, 10000]) to the non-linear signal in [0, 1].
double pq_signal(double nits) noexcept;

// BT.2100 HLG signal of an achromatic display luminance on the 1000 cd/m² reference display (inverse OOTF with
// system gamma 1.2, then the OETF); 1000 cd/m² and above give 1.0.
double hlg_signal(double nits) noexcept;

// sRGB signal of a luminance with SDR white at `sdr_white_nits` (203, BT.2408); brighter values clip to 1.0.
double srgb_signal(double nits, double sdr_white_nits = 203.0) noexcept;

// Full-range 10-bit code value of `nits` in `encoding`: round(E' * 1023).
std::uint16_t code10(OutputEncoding encoding, double nits) noexcept;

// One solid rectangle of the pattern, top-left origin, in pixels; colour as full-range 10-bit codes (R, G, B), already
// encoded for the output (the framebuffer gets code / 1023 as-is).
struct PatternRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  std::array<std::uint16_t, 3> code{};
};

// The luminances of the labelled patches (WI-0.052 goal) and the step count of the grey ramp.
inline constexpr std::array<int, 5> kPatchNits{100, 203, 1000, 4000, 10000};
inline constexpr int kRampSteps = 32;

// The PQ test pattern of WI-0.052 for a `width` x `height` surface, drawn in order over a black clear:
//   top band     a grey ramp of kRampSteps equal steps of the code value, 0 to 1023;
//   middle band  one grey patch per kPatchNits value, each labelled below with its luminance in digits;
//   bottom band  colour bars white, yellow, cyan, green, magenta, red, blue at 203 cd/m² per channel in the
//                output's primaries (BT.2020 for Pq10/Hlg10).
// Colours are encoded for `encoding`, so the SDR fallback shows the same layout, with everything above 203 cd/m²
// clipped to white. Returns an empty list for a surface smaller than 64 x 64.
std::vector<PatternRect> build_test_pattern(int width, int height, OutputEncoding encoding);

}  // namespace ovt::android_shell
