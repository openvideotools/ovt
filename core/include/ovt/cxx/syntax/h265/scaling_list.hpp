// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 scaling_list_data() (H.265 §7.3.4, §7.4.5): typed struct, parser and the default / flat lists (chapter 7
// §7.3.5 row 5, §7.3.7; WI-1.112). C++ helper header (D22): semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H265_SCALING_LIST_HPP
#define OVT_CXX_SYNTAX_H265_SCALING_LIST_HPP

#include <array>
#include <cstdint>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// Typed scaling_list_data() (§7.3.7). sizeId 0..3 = 4x4, 8x8, 16x16, 32x32; matrixId 0..5 (intra Y/Cb/Cr, inter
/// Y/Cb/Cr; 32x32 signals matrixId 0 and 3 only). ScalingList holds the coefficient lists in up-right diagonal scan
/// order (16 entries for sizeId 0, 64 otherwise), after prediction and default inference (§7.4.5). The ScalingFactor
/// arrays are the derived sample-position matrices, row-major [x * N + y] inside each matrixId block.
struct ScalingListData {
  std::array<std::array<bool, 6>, 4> scaling_list_pred_mode_flag{};
  std::array<std::array<uint32_t, 6>, 4> scaling_list_pred_matrix_id_delta{};
  std::array<std::array<int32_t, 6>, 2> scaling_list_dc_coef_minus8{};  // [sizeId - 2][matrixId]; inferred 8 → 16
  std::array<std::array<std::array<uint8_t, 64>, 6>, 4> ScalingList{};
  static constexpr size_t kMatrices = 6;
  std::array<uint8_t, kMatrices * 16> ScalingFactor4x4{};
  std::array<uint8_t, kMatrices * 64> ScalingFactor8x8{};
  std::array<uint8_t, kMatrices * 256> ScalingFactor16x16{};
  std::array<uint8_t, kMatrices * 1024>
      ScalingFactor32x32{};   // chroma matrixIds (and ScalingList[3][1,2,4,5]) copied from 16x16 (4:4:4 rule)
  bool from_default = false;  // Table 7-5 / 7-6 (scaling_list_enabled_flag without scaling_list_data)
  bool flat = false;          // all 16 (scaling_list_enabled_flag == 0)
  BitSpan span{0, 0};

  /// ScalingFactor[sizeId][matrixId][x][y] (§7.4.5); x and y < 4 << sizeId.
  [[nodiscard]] uint8_t scaling_factor(unsigned size_id, unsigned matrix_id, unsigned x, unsigned y) const noexcept;
};

/// Parses scaling_list_data() into the node `scaling_list_data` and derives ScalingList / ScalingFactor.
ScalingListData parse_scaling_list_data(BitReader& br, SyntaxBuilder& b);

/// The default lists of Table 7-5 / Table 7-6 (scaling_list_enabled_flag == 1, no scaling_list_data()), with
/// ScalingFactor derived.
[[nodiscard]] ScalingListData default_scaling_list_data();

/// The flat lists (all 16; scaling_list_enabled_flag == 0, §7.4.5 "m = 16"), with ScalingFactor derived.
[[nodiscard]] ScalingListData flat_scaling_list_data();

/// Recomputes the ScalingFactor arrays from ScalingList and the DC coefficients (§7.4.5, equations 7-40..7-45).
void derive_scaling_factors(ScalingListData& s);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_SCALING_LIST_HPP
