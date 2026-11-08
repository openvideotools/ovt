// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 scaling_list_data() parser and ScalingFactor derivation (H.265 §7.3.4, §7.4.5, §6.5.3; chapter 7 §7.3.5
// row 5, WI-1.112).

#include "ovt/cxx/syntax/h265/scaling_list.hpp"

#include <array>
#include <cstdint>
#include <utility>

#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

// Table 7-6: default values of ScalingList[1..3][matrixId][i], intra (matrixId 0..2) and inter (3..5), in
// up-right diagonal scan order. Table 7-5 (sizeId 0) is all 16.
constexpr std::array<uint8_t, 64> kDefaultIntra = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
                                                   17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
                                                   24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
                                                   29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115};
constexpr std::array<uint8_t, 64> kDefaultInter = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
                                                   18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
                                                   24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
                                                   28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91};

// §6.5.3 up-right diagonal scan order of a blkSize x blkSize block: diagScan[i] = (x, y).
template <size_t kBlkSize>
constexpr std::array<std::pair<uint8_t, uint8_t>, kBlkSize * kBlkSize> diag_scan() {
  std::array<std::pair<uint8_t, uint8_t>, kBlkSize * kBlkSize> scan{};
  unsigned i = 0;
  int x = 0;
  int y = 0;
  while (i < kBlkSize * kBlkSize) {
    while (y >= 0) {
      if (x < static_cast<int>(kBlkSize) && y < static_cast<int>(kBlkSize)) {
        scan[i] = {static_cast<uint8_t>(x), static_cast<uint8_t>(y)};
        ++i;
      }
      --y;
      ++x;
    }
    y = x;
    x = 0;
  }
  return scan;
}

constexpr auto kScan4 = diag_scan<4>();
constexpr auto kScan8 = diag_scan<8>();

constexpr unsigned coef_num(unsigned size_id) {
  return size_id == 0 ? 16U : 64U;
}

const std::array<uint8_t, 64>& default_list(unsigned size_id, unsigned matrix_id) {
  static constexpr std::array<uint8_t, 64> kFlat16 = [] {
    std::array<uint8_t, 64> a{};
    a.fill(16);
    return a;
  }();
  if (size_id == 0) {
    return kFlat16;
  }
  return matrix_id < 3 ? kDefaultIntra : kDefaultInter;
}

void fill_defaults(ScalingListData& s) {
  for (unsigned size_id = 0; size_id < 4; ++size_id) {
    for (unsigned matrix_id = 0; matrix_id < 6; ++matrix_id) {
      s.ScalingList[size_id][matrix_id] = default_list(size_id, matrix_id);
    }
  }
  s.scaling_list_dc_coef_minus8[0].fill(8);
  s.scaling_list_dc_coef_minus8[1].fill(8);
}

// Expands ScalingList[sizeId][matrixId] (64 coefficients over an 8x8 scan) into an N x N factor block with
// replication factor `rep` (2 for 16x16, 4 for 32x32) and the DC value at [0][0].
void expand(std::span<uint8_t> out, unsigned n, unsigned rep, const std::array<uint8_t, 64>& list, uint8_t dc) {
  for (unsigned i = 0; i < 64; ++i) {
    const auto [sx, sy] = kScan8[i];
    for (unsigned k = 0; k < rep; ++k) {
      for (unsigned j = 0; j < rep; ++j) {
        const unsigned x = sx * rep + k;
        const unsigned y = sy * rep + j;
        out[x * n + y] = list[i];
      }
    }
  }
  out[0] = dc;
}

void parse_one_list(BitReader& br, SyntaxBuilder& b, ScalingListData& s, unsigned size_id, unsigned matrix_id) {
  const bool pred_mode = b.read_flag("scaling_list_pred_mode_flag");
  s.scaling_list_pred_mode_flag[size_id][matrix_id] = pred_mode;
  const unsigned step = size_id == 3 ? 3U : 1U;
  if (!pred_mode) {
    const uint32_t delta = b.read_ue("scaling_list_pred_matrix_id_delta");
    s.scaling_list_pred_matrix_id_delta[size_id][matrix_id] = delta;
    detail::require_range(br, "scaling_list_pred_matrix_id_delta", delta, 0, matrix_id / step);
    if (delta == 0) {
      s.ScalingList[size_id][matrix_id] = default_list(size_id, matrix_id);
      if (size_id > 1) {
        s.scaling_list_dc_coef_minus8[size_id - 2][matrix_id] = 8;
      }
      b.derived("refMatrixId", std::string_view{"refMatrixId = default (Table 7-5 / 7-6)"});
    } else {
      const unsigned ref = matrix_id - delta * step;
      s.ScalingList[size_id][matrix_id] = s.ScalingList[size_id][ref];
      if (size_id > 1) {
        s.scaling_list_dc_coef_minus8[size_id - 2][matrix_id] = s.scaling_list_dc_coef_minus8[size_id - 2][ref];
      }
      b.derived("refMatrixId", ref);
    }
    return;
  }
  int32_t next_coef = 8;
  if (size_id > 1) {
    const int32_t dc = b.read_se("scaling_list_dc_coef_minus8");
    detail::check_range(b, "scaling_list_dc_coef_minus8", dc, -7, 247, "H.265 7.4.5");
    s.scaling_list_dc_coef_minus8[size_id - 2][matrix_id] = dc;
    next_coef = dc + 8;
  }
  const unsigned n = coef_num(size_id);
  for (unsigned i = 0; i < n; ++i) {
    const int32_t delta = detail::read_se_at(b, "scaling_list_delta_coef", static_cast<int>(i));
    detail::check_range(b, "scaling_list_delta_coef", delta, -128, 127, "H.265 7.4.5");
    next_coef = (next_coef + delta + 256) % 256;
    if (next_coef < 0) {  // a delta outside [-128, 127] on a corrupt stream; keep the value representable
      next_coef += 256;
    }
    s.ScalingList[size_id][matrix_id][i] = static_cast<uint8_t>(next_coef);
  }
}

}  // namespace

uint8_t ScalingListData::scaling_factor(unsigned size_id, unsigned matrix_id, unsigned x, unsigned y) const noexcept {
  switch (size_id) {
    case 0:
      return ScalingFactor4x4[matrix_id * 16 + x * 4 + y];
    case 1:
      return ScalingFactor8x8[matrix_id * 64 + x * 8 + y];
    case 2:
      return ScalingFactor16x16[matrix_id * 256 + x * 16 + y];
    default:
      return ScalingFactor32x32[matrix_id * 1024 + x * 32 + y];
  }
}

void derive_scaling_factors(ScalingListData& s) {
  for (unsigned m = 0; m < 6; ++m) {
    // 7-40: 4x4
    for (unsigned i = 0; i < 16; ++i) {
      const auto [x, y] = kScan4[i];
      s.ScalingFactor4x4[m * 16 + x * 4U + y] = s.ScalingList[0][m][i];
    }
    // 7-41: 8x8
    for (unsigned i = 0; i < 64; ++i) {
      const auto [x, y] = kScan8[i];
      s.ScalingFactor8x8[m * 64 + x * 8U + y] = s.ScalingList[1][m][i];
    }
    // 7-42, 7-43: 16x16 from the 8x8 scan replicated 2x2, DC from scaling_list_dc_coef_minus8[0]
    expand(std::span<uint8_t>(s.ScalingFactor16x16).subspan(size_t{m} * 256, 256), 16, 2, s.ScalingList[2][m],
           static_cast<uint8_t>(s.scaling_list_dc_coef_minus8[0][m] + 8));
    // 7-44, 7-45: 32x32 replicated 4x4; matrixId 0 and 3 from the 32x32 lists, the chroma matrixIds (used only when
    // ChromaArrayType == 3) from the 16x16 lists and their DC coefficients
    const bool signalled = (m % 3) == 0;
    expand(std::span<uint8_t>(s.ScalingFactor32x32).subspan(size_t{m} * 1024, 1024), 32, 4,
           signalled ? s.ScalingList[3][m] : s.ScalingList[2][m],
           static_cast<uint8_t>(s.scaling_list_dc_coef_minus8[signalled ? 1 : 0][m] + 8));
  }
}

ScalingListData default_scaling_list_data() {
  ScalingListData s;
  fill_defaults(s);
  s.from_default = true;
  derive_scaling_factors(s);
  return s;
}

ScalingListData flat_scaling_list_data() {
  ScalingListData s;
  for (auto& per_size : s.ScalingList) {
    for (auto& list : per_size) {
      list.fill(16);
    }
  }
  s.scaling_list_dc_coef_minus8[0].fill(8);
  s.scaling_list_dc_coef_minus8[1].fill(8);
  s.flat = true;
  derive_scaling_factors(s);
  return s;
}

ScalingListData parse_scaling_list_data(BitReader& br, SyntaxBuilder& b) {
  ScalingListData s;
  fill_defaults(s);  // lists that are not signalled (32x32 chroma) keep their defaults
  const uint64_t start = br.bit_pos();
  {
    auto scope = b.scoped("scaling_list_data", "H.265 7.3.4");
    for (unsigned size_id = 0; size_id < 4; ++size_id) {
      b.begin_array_element("sizeId", static_cast<int>(size_id));
      for (unsigned matrix_id = 0; matrix_id < 6; matrix_id += (size_id == 3) ? 3U : 1U) {
        b.begin_array_element("matrixId", static_cast<int>(matrix_id));
        parse_one_list(br, b, s, size_id, matrix_id);
        b.end_array_element();
      }
      b.end_array_element();
    }
  }
  derive_scaling_factors(s);
  s.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return s;
}

}  // namespace ovt::syntax::h265
