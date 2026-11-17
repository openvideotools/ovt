// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// CPU colour math of the pixel probe (chapter 10 §10.5.7, §10.6.3; WI-2.435 step 5).

#include "color/cpu_convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <libplacebo/colorspace.h>

namespace ovt::render::detail {
namespace {

// ST 2084 constants.
constexpr double kM1 = 2610.0 / 16384.0;
constexpr double kM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kC1 = 3424.0 / 4096.0;
constexpr double kC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kC3 = 2392.0 / 4096.0 * 32.0;

// BT.2100 Table 5 HLG constants; the OOTF with Lw = 1000 cd/m², Lb = 0 and system gamma 1.2 (§10.5.7).
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 1.0 - (4.0 * kHlgA);
constexpr double kHlgC = 0.55991073;  // 0.5 - a * ln(4a)
constexpr double kHlgPeakNits = 1000.0;
constexpr double kHlgGamma = 1.2;

struct LumaWeights {
  double kr;
  double kb;
};

// H.273 Table 4 KR / KB of the YCbCr matrices the probe converts.
bool luma_weights(pl_color_system sys, LumaWeights& w) {
  switch (sys) {
    case PL_COLOR_SYSTEM_BT_601:
      w = {0.299, 0.114};
      return true;
    case PL_COLOR_SYSTEM_UNKNOWN:  // the §10.3.3 tables resolve unknown codes; BT.709 if one slips through
    case PL_COLOR_SYSTEM_BT_709:
      w = {0.2126, 0.0722};
      return true;
    case PL_COLOR_SYSTEM_SMPTE_240M:
      w = {0.212, 0.087};
      return true;
    case PL_COLOR_SYSTEM_BT_2020_NC:
      w = {0.2627, 0.0593};
      return true;
    default:
      return false;
  }
}

// Gamma of the pure-power and BT.1886 (Lb = 0, so V^2.4) curves; 0 = not a power curve.
double power_gamma(pl_color_transfer trc) {
  switch (trc) {
    case PL_COLOR_TRC_UNKNOWN:
    case PL_COLOR_TRC_BT_1886:
    case PL_COLOR_TRC_GAMMA24:
      return 2.4;
    case PL_COLOR_TRC_GAMMA18:
      return 1.8;
    case PL_COLOR_TRC_GAMMA20:
      return 2.0;
    case PL_COLOR_TRC_GAMMA22:
      return 2.2;
    case PL_COLOR_TRC_GAMMA26:
      return 2.6;
    case PL_COLOR_TRC_GAMMA28:
      return 2.8;
    default:
      return 0.0;
  }
}

double srgb_eotf(double v) {
  return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

}  // namespace

float pq_eotf_nits(float signal) {
  const double e = std::pow(std::clamp(static_cast<double>(signal), 0.0, 1.0), 1.0 / kM2);
  const double y = std::pow(std::max(e - kC1, 0.0) / (kC2 - (kC3 * e)), 1.0 / kM1);
  return static_cast<float>(y * 10000.0);
}

float pq_oetf(float nits) {
  const double y = std::pow(std::clamp(static_cast<double>(nits) / 10000.0, 0.0, 1.0), kM1);
  return static_cast<float>(std::pow((kC1 + (kC2 * y)) / (1.0 + (kC3 * y)), kM2));
}

float hlg_inverse_oetf(float signal) {
  const double e = std::clamp(static_cast<double>(signal), 0.0, 1.0);
  const double v = e <= 0.5 ? (e * e) / 3.0 : (std::exp((e - kHlgC) / kHlgA) + kHlgB) / 12.0;
  return static_cast<float>(v);
}

bool code_to_nonlinear_rgb(const CodeColor& color, const std::array<int32_t, 3>& code, std::array<float, 3>& rgb) {
  const int b = std::clamp(color.bit_depth, 8, 16);
  const double scale = std::ldexp(1.0, b - 8);  // 2^(b-8)
  const double max_code = std::ldexp(1.0, b) - 1.0;
  const bool rgb_sys = color.sys == PL_COLOR_SYSTEM_RGB;
  // RGB and unknown levels default as libplacebo does: full for RGB, limited for YCbCr.
  const bool full = color.levels == PL_COLOR_LEVELS_FULL || (color.levels == PL_COLOR_LEVELS_UNKNOWN && rgb_sys);
  const auto luma = [&](int32_t c) { return full ? c / max_code : (c - (16.0 * scale)) / (219.0 * scale); };
  const auto chroma = [&](int32_t c) {
    if (c < 0) {
      return 0.0;  // 4:0:0: neutral
    }
    return full ? (c - std::ldexp(1.0, b - 1)) / max_code : (c - (128.0 * scale)) / (224.0 * scale);
  };
  if (rgb_sys) {
    // Planes G, B, R (H.273 matrix 0), each quantised like luma.
    rgb = {static_cast<float>(luma(code[2])), static_cast<float>(luma(code[0])), static_cast<float>(luma(code[1]))};
    return true;
  }
  const double y = luma(code[0]);
  const double cb = chroma(code[1]);
  const double cr = chroma(code[2]);
  if (color.sys == PL_COLOR_SYSTEM_YCGCO) {
    const double t = y - cb;  // Cg in the Cb slot, Co in the Cr slot (H.273 Eq. 50-52)
    rgb = {static_cast<float>(t + cr), static_cast<float>(y + cb), static_cast<float>(t - cr)};
    return true;
  }
  LumaWeights w{};
  if (!luma_weights(color.sys, w)) {
    return false;
  }
  const double r = y + (2.0 * (1.0 - w.kr) * cr);
  const double bl = y + (2.0 * (1.0 - w.kb) * cb);
  const double g = (y - (w.kr * r) - (w.kb * bl)) / (1.0 - w.kr - w.kb);
  rgb = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(bl)};
  return true;
}

bool code_to_light(const CodeColor& color, const std::array<int32_t, 3>& code, ProbeLight& out) {
  constexpr double kWhite = kSdrWhiteNits;
  out = ProbeLight{};
  std::array<float, 3> e{};
  if (!code_to_nonlinear_rgb(color, code, e)) {
    return false;
  }
  const pl_raw_primaries* raw =
      pl_raw_primaries_get(color.primaries == PL_COLOR_PRIM_UNKNOWN ? PL_COLOR_PRIM_BT_709 : color.primaries);
  const pl_matrix3x3 to_xyz = pl_get_rgb2xyz_matrix(raw);
  const auto row = [&](int i, const std::array<double, 3>& v) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): fixed 3x3 matrix of libplacebo
    return (double{to_xyz.m[i][0]} * v[0]) + (double{to_xyz.m[i][1]} * v[1]) + (double{to_xyz.m[i][2]} * v[2]);
  };

  // Linear light per channel, 1.0 = 203 nits.
  std::array<double, 3> lin{};
  const double gamma = power_gamma(color.transfer);
  if (color.transfer == PL_COLOR_TRC_PQ) {
    for (size_t i = 0; i < 3; ++i) {
      lin.at(i) = double{pq_eotf_nits(e.at(i))} / kWhite;
    }
  } else if (color.transfer == PL_COLOR_TRC_HLG) {
    std::array<double, 3> scene{};
    for (size_t i = 0; i < 3; ++i) {
      scene.at(i) = hlg_inverse_oetf(e.at(i));
    }
    const double ys = std::max(row(1, scene), 0.0);  // scene luminance with the content primaries' Y row
    const double ootf = kHlgPeakNits * (ys > 0.0 ? std::pow(ys, kHlgGamma - 1.0) : 0.0);
    for (size_t i = 0; i < 3; ++i) {
      lin.at(i) = ootf * scene.at(i) / kWhite;
    }
  } else if (color.transfer == PL_COLOR_TRC_SRGB) {
    for (size_t i = 0; i < 3; ++i) {
      lin.at(i) = srgb_eotf(std::clamp(static_cast<double>(e.at(i)), 0.0, 1.0));
    }
  } else if (color.transfer == PL_COLOR_TRC_LINEAR) {
    for (size_t i = 0; i < 3; ++i) {
      lin.at(i) = e.at(i);
    }
  } else if (gamma > 0.0) {
    for (size_t i = 0; i < 3; ++i) {
      lin.at(i) = std::pow(std::clamp(static_cast<double>(e.at(i)), 0.0, 1.0), gamma);
    }
  } else {
    return false;
  }

  const double x = row(0, lin);
  const double yy = row(1, lin);
  const double z = row(2, lin);
  const double maxrgb = std::max({lin[0], lin[1], lin[2]});
  for (size_t i = 0; i < 3; ++i) {
    out.rgb_linear.at(i) = static_cast<float>(lin.at(i));
  }
  out.nits_luminance = static_cast<float>(yy * kWhite);
  out.nits_maxrgb = static_cast<float>(maxrgb * kWhite);
  out.pq_code = pq_oetf(out.nits_maxrgb);
  const double sum = x + yy + z;
  if (sum > 0.0) {
    out.cie_x = static_cast<float>(x / sum);
    out.cie_y = static_cast<float>(yy / sum);
  }
  return true;
}

}  // namespace ovt::render::detail
