// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.439 acceptance tests 2 and 3 (chapter 10 §10.5.2): WCAG contrast of the overlay line/arrow/label colours and
// the halo rule, and the colour-vision-deficiency check of the colour-blind-safe palette. CPU only.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>

#include "ovt/render/overlay_colors.h"

namespace {

using Rgb = std::array<double, 3>;
using Mat3 = std::array<Rgb, 3>;

double srgb_to_linear(double v) {
  return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

Rgb linear_rgb(uint32_t rgba) {
  return {srgb_to_linear(static_cast<double>((rgba >> 24U) & 0xFFU) / 255.0),
          srgb_to_linear(static_cast<double>((rgba >> 16U) & 0xFFU) / 255.0),
          srgb_to_linear(static_cast<double>((rgba >> 8U) & 0xFFU) / 255.0)};
}

double alpha_of(uint32_t rgba) {
  return static_cast<double>(rgba & 0xFFU) / 255.0;
}

// WCAG 2.x relative luminance of linear BT.709 RGB.
double luminance(const Rgb& lin) {
  return (0.2126 * lin[0]) + (0.7152 * lin[1]) + (0.0722 * lin[2]);
}

double contrast(double la, double lb) {
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Straight-alpha "over" in linear light, as libplacebo composites the overlay layer (§10.5).
Rgb over(const Rgb& fg, double alpha, const Rgb& bg) {
  Rgb out{};
  for (size_t i = 0; i < 3; ++i) {
    out.at(i) = (fg.at(i) * alpha) + (bg.at(i) * (1.0 - alpha));
  }
  return out;
}

Rgb grey(int code) {
  const double v = srgb_to_linear(static_cast<double>(code) / 255.0);
  return {v, v, v};
}

// CIE L*a*b* (D65) of linear BT.709 RGB.
Rgb to_lab(const Rgb& lin) {
  const double x = ((0.4124564 * lin[0]) + (0.3575761 * lin[1]) + (0.1804375 * lin[2])) / 0.95047;
  const double y = (0.2126729 * lin[0]) + (0.7151522 * lin[1]) + (0.0721750 * lin[2]);
  const double z = ((0.0193339 * lin[0]) + (0.1191920 * lin[1]) + (0.9503041 * lin[2])) / 1.08883;
  const auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : ((24389.0 / 27.0 * t) + 16.0) / 116.0; };
  return {(116.0 * f(y)) - 16.0, 500.0 * (f(x) - f(y)), 200.0 * (f(y) - f(z))};
}

double deg(double rad) {
  return rad * 180.0 / std::numbers::pi;
}

double rad(double deg) {
  return deg * std::numbers::pi / 180.0;
}

// CIEDE2000 (Sharma, Wu, Dalal 2005).
double delta_e2000(const Rgb& lab1, const Rgb& lab2) {
  const double c1 = std::hypot(lab1[1], lab1[2]);
  const double c2 = std::hypot(lab2[1], lab2[2]);
  const double cb7 = std::pow((c1 + c2) / 2.0, 7.0);
  const double g = 0.5 * (1.0 - std::sqrt(cb7 / (cb7 + std::pow(25.0, 7.0))));
  const double a1 = (1.0 + g) * lab1[1];
  const double a2 = (1.0 + g) * lab2[1];
  const double c1p = std::hypot(a1, lab1[2]);
  const double c2p = std::hypot(a2, lab2[2]);
  const auto hue = [](double b, double a) {
    if (a == 0.0 && b == 0.0) {
      return 0.0;
    }
    const double h = deg(std::atan2(b, a));
    return h < 0.0 ? h + 360.0 : h;
  };
  const double h1 = hue(lab1[2], a1);
  const double h2 = hue(lab2[2], a2);
  const double dl = lab2[0] - lab1[0];
  const double dc = c2p - c1p;
  double dh = 0.0;
  if (c1p * c2p != 0.0) {
    dh = h2 - h1;
    if (dh > 180.0) {
      dh -= 360.0;
    } else if (dh < -180.0) {
      dh += 360.0;
    }
  }
  const double dhh = 2.0 * std::sqrt(c1p * c2p) * std::sin(rad(dh / 2.0));
  const double lbar = (lab1[0] + lab2[0]) / 2.0;
  const double cbar = (c1p + c2p) / 2.0;
  double hbar = h1 + h2;
  if (c1p * c2p != 0.0) {
    if (std::abs(h1 - h2) <= 180.0) {
      hbar = (h1 + h2) / 2.0;
    } else {
      hbar = (h1 + h2 < 360.0) ? (h1 + h2 + 360.0) / 2.0 : (h1 + h2 - 360.0) / 2.0;
    }
  }
  const double t = 1.0 - (0.17 * std::cos(rad(hbar - 30.0))) + (0.24 * std::cos(rad(2.0 * hbar))) +
                   (0.32 * std::cos(rad((3.0 * hbar) + 6.0))) - (0.20 * std::cos(rad((4.0 * hbar) - 63.0)));
  const double dtheta = 30.0 * std::exp(-std::pow((hbar - 275.0) / 25.0, 2.0));
  const double cbar7 = std::pow(cbar, 7.0);
  const double rc = 2.0 * std::sqrt(cbar7 / (cbar7 + std::pow(25.0, 7.0)));
  const double sl = 1.0 + (0.015 * std::pow(lbar - 50.0, 2.0) / std::sqrt(20.0 + std::pow(lbar - 50.0, 2.0)));
  const double sc = 1.0 + (0.045 * cbar);
  const double sh = 1.0 + (0.015 * cbar * t);
  const double rt = -std::sin(rad(2.0 * dtheta)) * rc;
  const double tl = dl / sl;
  const double tc = dc / sc;
  const double th = dhh / sh;
  return std::sqrt((tl * tl) + (tc * tc) + (th * th) + (rt * tc * th));
}

// Machado, Oliveira, Fernandes 2009, severity 1.0, applied to linear RGB and clamped to [0, 1].
constexpr Mat3 kProtanopia = {
    {{0.152286, 1.052583, -0.204868}, {0.114503, 0.786281, 0.099216}, {-0.003882, -0.048116, 1.051998}}};
constexpr Mat3 kDeuteranopia = {
    {{0.367322, 0.860646, -0.227968}, {0.280085, 0.672501, 0.047413}, {-0.011820, 0.042940, 0.968881}}};

Rgb simulate(const Mat3& m, const Rgb& lin) {
  Rgb out{};
  for (size_t r = 0; r < 3; ++r) {
    const double v = (m.at(r)[0] * lin[0]) + (m.at(r)[1] * lin[1]) + (m.at(r)[2] * lin[2]);
    out.at(r) = std::clamp(v, 0.0, 1.0);
  }
  return out;
}

// Rows of §10.5.2 that are drawn as lines, arrows or labels (WI-2.439 AT2).
constexpr std::array<size_t, 17> kLineRows = {1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};

struct Backgrounds {
  const char* name;
  Rgb dark;   // average of the 10 % darkest luma pixels
  Rgb light;  // average of the 10 % lightest luma pixels
};

// SPEC-ASSUMPTION(#634): the 12 golden frames of §10.9.2 named by AT2 do not exist yet (the manifest has no
// render-ref entries), so the dark/light averages are the extremes any SdrPng8 frame can reach: full-range black and
// white, and video-range black and white (16 and 235), which real content's 10 % averages sit close to.
const std::array<Backgrounds, 2> kBackgrounds = {{
    {"full-range black/white", grey(0), grey(255)},
    {"video-range 16/235", grey(16), grey(235)},
}};

}  // namespace

TEST_CASE("render_test_overlay_contrast: CIEDE2000 reference pairs (Sharma 2005)", "[fast][render][overlay]") {
  using Catch::Matchers::WithinAbs;
  CHECK_THAT(delta_e2000({50.0, 2.6772, -79.7751}, {50.0, 0.0, -82.7485}), WithinAbs(2.0425, 1e-4));
  CHECK_THAT(delta_e2000({50.0, 2.5, 0.0}, {73.0, 25.0, -18.0}), WithinAbs(27.1492, 1e-4));
  CHECK_THAT(delta_e2000({60.2574, -34.0099, 36.2677}, {60.4626, -34.1751, 39.4387}), WithinAbs(1.2644, 1e-4));
  CHECK_THAT(delta_e2000({2.0776, 0.0795, -1.1350}, {0.9033, -0.0636, -0.5514}), WithinAbs(0.9082, 1e-4));
}

// AT2: every line/arrow/label colour, composited at its table alpha, has >= 3:1 against at least one of the two
// averages, and every colour that fails against either average has its halo set.
TEST_CASE("render_test_overlay_contrast: line colours and halo rule (WI-2.439 AT2)", "[fast][render][overlay]") {
  struct Palette {
    const char* name;
    const std::array<uint32_t, 33>* colors;
    const std::array<uint32_t, 33>* halo;
  };
  const std::array<Palette, 2> palettes = {{
      {"kOverlayColors", &ovt::render::kOverlayColors, &ovt::render::kOverlayHalo},
      {"kOverlayColorsCvd", &ovt::render::kOverlayColorsCvd, &ovt::render::kOverlayHaloCvd},
  }};
  for (const Palette& pal : palettes) {
    for (const Backgrounds& bg : kBackgrounds) {
      for (const size_t row : kLineRows) {
        const uint32_t rgba = pal.colors->at(row);
        const Rgb fg = linear_rgb(rgba);
        const double on_dark = contrast(luminance(over(fg, alpha_of(rgba), bg.dark)), luminance(bg.dark));
        const double on_light = contrast(luminance(over(fg, alpha_of(rgba), bg.light)), luminance(bg.light));
        INFO(pal.name << " row " << row << " on " << bg.name << ": " << on_dark << " / " << on_light);
        CHECK(std::max(on_dark, on_light) >= 3.0);
        if (std::min(on_dark, on_light) < 3.0) {
          const uint32_t halo = pal.halo->at(row);
          CHECK((halo & 0xFFU) != 0U);
          // The halo is #000000 or #FFFFFF, the one that contrasts with the colour itself.
          const uint32_t halo_rgb = halo >> 8U;
          CHECK((halo_rgb == 0U || halo_rgb == 0xFFFFFFU));
          const double colour_l = luminance(fg);
          const double halo_l = halo_rgb == 0U ? 0.0 : 1.0;
          CHECK(contrast(colour_l, halo_l) >= contrast(colour_l, 1.0 - halo_l));
        }
      }
    }
  }
  // §10.5.2 rows 15 and 19 carry their own outer outline.
  CHECK(ovt::render::kOverlayHalo[15] == 0x000000FFU);
  CHECK(ovt::render::kOverlayHalo[19] == 0x000000CCU);
  CHECK(ovt::render::kOverlayHaloCvd[15] == 0x000000FFU);
  CHECK(ovt::render::kOverlayHaloCvd[19] == 0x000000CCU);
  // Fills, hatch, ref slots and frame types have no halo.
  for (const size_t row :
       {size_t{0}, size_t{4}, size_t{5}, size_t{6}, size_t{7}, size_t{22}, size_t{29}, size_t{30}, size_t{32}}) {
    CHECK(ovt::render::kOverlayHalo.at(row) == 0U);
    CHECK(ovt::render::kOverlayHaloCvd.at(row) == 0U);
  }
}

// AT3: after protanopia and deuteranopia simulation the four coded roles of kOverlayColorsCvd (L0, L1, bi, intra) stay
// pairwise >= 15 ΔE2000 apart (and without simulation too).
TEST_CASE("render_test_overlay_contrast: CVD palette roles stay distinguishable (WI-2.439 AT3)",
          "[fast][render][overlay]") {
  const auto& cvd = ovt::render::kOverlayColorsCvd;
  const std::array<size_t, 4> roles = {1, 2, 3, 0};  // L0, L1, bi, intra
  const std::array<const char*, 4> role_names = {"L0", "L1", "bi", "intra"};
  struct Sim {
    const char* name;
    const Mat3* m;
  };
  const std::array<Sim, 3> sims = {{{"none", nullptr}, {"protanopia", &kProtanopia}, {"deuteranopia", &kDeuteranopia}}};
  for (const Sim& sim : sims) {
    for (size_t a = 0; a < roles.size(); ++a) {
      for (size_t b = a + 1; b < roles.size(); ++b) {
        Rgb la = linear_rgb(cvd.at(roles.at(a)));
        Rgb lb = linear_rgb(cvd.at(roles.at(b)));
        if (sim.m != nullptr) {
          la = simulate(*sim.m, la);
          lb = simulate(*sim.m, lb);
        }
        const double de = delta_e2000(to_lab(la), to_lab(lb));
        INFO(sim.name << ": " << role_names.at(a) << " vs " << role_names.at(b) << " ΔE2000 " << de);
        CHECK(de >= 15.0);
      }
    }
  }
  // The intra stroke (row 20) uses the intra role colour at full alpha.
  CHECK((cvd[20] >> 8U) == (cvd[0] >> 8U));
  // Okabe-Ito values of §10.5.2.
  CHECK((cvd[1] >> 8U) == 0xE69F00U);
  CHECK((cvd[2] >> 8U) == 0x56B4E9U);
  CHECK((cvd[0] >> 8U) == 0x009E73U);
}
