// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.273 (07/2024) Tables 2, 3 and 4 names. The strings are OVT's own short labels of the referenced standards.

#include "syntax/common/h273_names.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ovt::syntax {

namespace {

template <size_t N>
std::string_view lookup(const std::array<std::pair<uint32_t, std::string_view>, N>& table, uint32_t code) noexcept {
  for (const auto& [c, name] : table) {
    if (c == code) {
      return name;
    }
  }
  return {};
}

}  // namespace

std::string_view h273_colour_primaries_name(uint32_t code) noexcept {
  static constexpr std::array<std::pair<uint32_t, std::string_view>, 12> kNames{{
      {1, "BT.709"},
      {2, "Unspecified"},
      {4, "BT.470 System M"},
      {5, "BT.470 System B/G"},
      {6, "SMPTE 170M"},
      {7, "SMPTE 240M"},
      {8, "Generic film"},
      {9, "BT.2020"},
      {10, "SMPTE ST 428-1 (XYZ)"},
      {11, "SMPTE RP 431-2 (P3-DCI)"},
      {12, "SMPTE EG 432-1 (P3-D65)"},
      {22, "EBU Tech 3213-E"},
  }};
  return lookup(kNames, code);
}

std::string_view h273_transfer_characteristics_name(uint32_t code) noexcept {
  static constexpr std::array<std::pair<uint32_t, std::string_view>, 17> kNames{{
      {1, "BT.709"},
      {2, "Unspecified"},
      {4, "BT.470 System M (gamma 2.2)"},
      {5, "BT.470 System B/G (gamma 2.8)"},
      {6, "SMPTE 170M"},
      {7, "SMPTE 240M"},
      {8, "Linear"},
      {9, "Logarithmic (100:1)"},
      {10, "Logarithmic (100*Sqrt(10):1)"},
      {11, "IEC 61966-2-4"},
      {12, "BT.1361"},
      {13, "sRGB / IEC 61966-2-1"},
      {14, "BT.2020 (10-bit)"},
      {15, "BT.2020 (12-bit)"},
      {16, "SMPTE ST 2084 (PQ)"},
      {17, "SMPTE ST 428-1"},
      {18, "ARIB STD-B67 (HLG)"},
  }};
  return lookup(kNames, code);
}

std::string_view h273_matrix_coefficients_name(uint32_t code) noexcept {
  static constexpr std::array<std::pair<uint32_t, std::string_view>, 14> kNames{{
      {0, "Identity (GBR)"},
      {1, "BT.709"},
      {2, "Unspecified"},
      {4, "FCC"},
      {5, "BT.470 System B/G"},
      {6, "SMPTE 170M"},
      {7, "SMPTE 240M"},
      {8, "YCgCo"},
      {9, "BT.2020 non-constant luminance"},
      {10, "BT.2020 constant luminance"},
      {11, "SMPTE ST 2085"},
      {12, "Chromaticity-derived non-constant luminance"},
      {13, "Chromaticity-derived constant luminance"},
      {14, "ICtCp"},
  }};
  return lookup(kNames, code);
}

}  // namespace ovt::syntax
