// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// §10.7.7 overlay point tables (WI-4.454): the 100 % primaries and secondaries of BT.2020, BT.709 and P3 (D65) as
// BT.2020 (Cb, Cr) of the PQ-encoded signal for the vectorscope, and the gamut triangles and white point in CIE 1931
// xy for the CIE xy scope (the spectral locus is in cie1931_locus.h).
//
// The (Cb, Cr) values were computed once in double precision: linear RGB of the gamut (1.0 = 10 000 nits) -> XYZ ->
// BT.2020 RGB with the normalised primary matrices of SMPTE RP 177 from the xy values below (D65 white), then
// R'G'B' = PQ(rgb) (ST 2084), Y' = 0.2627 R' + 0.6780 G' + 0.0593 B', Cb = (B' - Y') / 1.8814, Cr = (R' - Y') / 1.4746
// (§10.7.7). render/tests/test_scopes.cpp recomputes them with libplacebo's matrices and vectorscope_cbcr().

#include <array>

#include "measure/scopes.h"

namespace ovt::render::detail {

namespace {

// R, Y, G, C, B, M at 100 %.
constexpr std::array<CbCr, 6> kTargets2020 = {{
    {-0.139630F, 0.499999F},
    {-0.499999F, 0.040214F},
    {-0.360370F, -0.459785F},
    {0.139630F, -0.499999F},
    {0.500000F, -0.040214F},
    {0.360370F, 0.459785F},
}};
constexpr std::array<CbCr, 6> kTargets709 = {{
    {-0.109972F, 0.125750F},
    {-0.120658F, 0.008022F},
    {-0.111337F, -0.044632F},
    {0.016511F, -0.049019F},
    {0.214169F, 0.051287F},
    {0.099018F, 0.104323F},
}};
constexpr std::array<CbCr, 6> kTargetsP3 = {{
    {-0.375843F, 0.178456F},
    {-0.219587F, 0.015808F},
    {-0.190614F, -0.066321F},
    {0.022709F, -0.072733F},
    {0.214249F, 0.051560F},
    {0.112939F, 0.129576F},
}};

// Rec. ITU-R BT.709-6, SMPTE EG 432-1 (P3 with the D65 white of Display P3), Rec. ITU-R BT.2020-2.
constexpr std::array<Chromaticity, 3> kPrimaries709 = {{{0.640F, 0.330F}, {0.300F, 0.600F}, {0.150F, 0.060F}}};
constexpr std::array<Chromaticity, 3> kPrimariesP3 = {{{0.680F, 0.320F}, {0.265F, 0.690F}, {0.150F, 0.060F}}};
constexpr std::array<Chromaticity, 3> kPrimaries2020 = {{{0.708F, 0.292F}, {0.170F, 0.797F}, {0.131F, 0.046F}}};
constexpr Chromaticity kD65 = {0.3127F, 0.3290F};

}  // namespace

const std::array<CbCr, 6>& vectorscope_targets(Gamut gamut) {
  switch (gamut) {
    case Gamut::Bt709:
      return kTargets709;
    case Gamut::P3D65:
      return kTargetsP3;
    case Gamut::Bt2020:
      break;
  }
  return kTargets2020;
}

const std::array<Chromaticity, 3>& gamut_primaries_xy(Gamut gamut) {
  switch (gamut) {
    case Gamut::Bt709:
      return kPrimaries709;
    case Gamut::P3D65:
      return kPrimariesP3;
    case Gamut::Bt2020:
      break;
  }
  return kPrimaries2020;
}

Chromaticity d65_white_xy() {
  return kD65;
}

}  // namespace ovt::render::detail
