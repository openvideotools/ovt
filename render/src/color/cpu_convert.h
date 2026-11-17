// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// CPU colour math of the pixel probe (chapter 10 §10.5.7, §10.6.3; WI-2.435 step 5): stored code values -> R'G'B' ->
// linear light -> XYZ, nits, PQ and xy. The colour description is the one the surface already mapped with the §10.3.3
// tables (FrameSlot.frame.repr / .color), so the probe and the GPU path read a picture the same way. Shared with the
// §10.7 measurement tests. Internal to libovt-render; exported only for render/tests.

#ifndef OVT_RENDER_SRC_COLOR_CPU_CONVERT_H
#define OVT_RENDER_SRC_COLOR_CPU_CONVERT_H

#include <array>
#include <cstdint>

#include <libplacebo/colorspace.h>

#include "ovt/render/render_api.h"

namespace ovt::render::detail {

inline constexpr float kSdrWhiteNits = 203.0F;  // §10.6.3: rgb_linear 1.0 = 203 nits; SDR peak assumption

struct CodeColor {
  pl_color_system sys = PL_COLOR_SYSTEM_BT_709;
  pl_color_levels levels = PL_COLOR_LEVELS_LIMITED;
  pl_color_primaries primaries = PL_COLOR_PRIM_BT_709;
  pl_color_transfer transfer = PL_COLOR_TRC_BT_1886;
  int bit_depth = 8;
};

struct ProbeLight {
  std::array<float, 3> rgb_linear{};  // content primaries, 1.0 = 203 nits
  float nits_luminance = -1;
  float nits_maxrgb = -1;
  float pq_code = -1;
  float cie_x = 0;
  float cie_y = 0;
};

// SMPTE ST 2084 (Eq. 4.1 EOTF and its inverse): signal in [0, 1] <-> cd/m² in [0, 10000].
OVT_RENDER_API float pq_eotf_nits(float signal);
OVT_RENDER_API float pq_oetf(float nits);

// BT.2100 Table 5 HLG inverse OETF: signal in [0, 1] -> normalised scene light in [0, 1].
OVT_RENDER_API float hlg_inverse_oetf(float signal);

// Non-linear R'G'B' in [0, 1] (nominal) from the three code values of `code` (Y, Cb, Cr; for RGB the planes G, B, R;
// a chroma value < 0 means "no chroma plane" and reads as neutral). H.273 quantisation: limited range
// Y = (code - 16 * 2^(b-8)) / (219 * 2^(b-8)), C = (code - 128 * 2^(b-8)) / (224 * 2^(b-8)); full range
// Y = code / (2^b - 1), C = (code - 2^(b-1)) / (2^b - 1). False for colour systems the probe does not convert
// (BT.2020 constant luminance, ICtCp, XYZ, Dolby Vision).
OVT_RENDER_API bool code_to_nonlinear_rgb(const CodeColor& color, const std::array<int32_t, 3>& code,
                                          std::array<float, 3>& rgb);

// The whole §10.6.3 read-out. False (and `out` left at its "unavailable" defaults) when the colour system or the
// transfer function is not one the probe converts.
OVT_RENDER_API bool code_to_light(const CodeColor& color, const std::array<int32_t, 3>& code, ProbeLight& out);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_COLOR_CPU_CONVERT_H
