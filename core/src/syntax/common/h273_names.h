// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ITU-T H.273 code-point names (ColourPrimaries, TransferCharacteristics, MatrixCoefficients), shared by the
// H.264 VUI (Tables E-3..E-5), the H.265 VUI and the AV1 color_config (chapter 7 §7.3.4 row 4). Module-internal.

#ifndef OVT_SRC_SYNTAX_COMMON_H273_NAMES_H
#define OVT_SRC_SYNTAX_COMMON_H273_NAMES_H

#include <cstdint>
#include <string_view>

namespace ovt::syntax {

/// Empty when the code point is reserved; "Unspecified" for 2.
[[nodiscard]] std::string_view h273_colour_primaries_name(uint32_t code) noexcept;
[[nodiscard]] std::string_view h273_transfer_characteristics_name(uint32_t code) noexcept;
[[nodiscard]] std::string_view h273_matrix_coefficients_name(uint32_t code) noexcept;

}  // namespace ovt::syntax

#endif  // OVT_SRC_SYNTAX_COMMON_H273_NAMES_H
