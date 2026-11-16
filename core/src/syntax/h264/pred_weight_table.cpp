// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 pred_weight_table() (§7.3.3.2) with the inferred weights of §7.4.3.2: chapter 7 §7.3.4 row 26, WI-1.109.
// Element order follows H.264 (08/2021) §7.3.3.2 line by line; element names and indices follow trace_headers
// (`luma_weight_l0[i]`, `chroma_weight_l0[i][j]`).

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

struct ListNames {
  std::string_view luma_weight_flag;
  std::string_view luma_weight;
  std::string_view luma_offset;
  std::string_view chroma_weight_flag;
  std::string_view chroma_weight;
  std::string_view chroma_offset;
};

constexpr ListNames kL0{"luma_weight_l0_flag",   "luma_weight_l0",   "luma_offset_l0",
                        "chroma_weight_l0_flag", "chroma_weight_l0", "chroma_offset_l0"};
constexpr ListNames kL1{"luma_weight_l1_flag",   "luma_weight_l1",   "luma_offset_l1",
                        "chroma_weight_l1_flag", "chroma_weight_l1", "chroma_offset_l1"};

// §7.4.3.2: every weight and offset is in -128..127. They only enter the weighted-prediction formulas of later
// modules, so a violation is reported and parsing continues.
void check_weight(SyntaxBuilder& b, std::string_view name, int32_t v) {
  detail::check_range(b, name, v, -128, 127, "H.264 7.4.3.2");
}

void read_list(SyntaxBuilder& b, const ListNames& n, uint32_t count, uint32_t luma_denom, uint32_t chroma_denom,
               bool chroma, PredWeightTable::List& l) {
  const int32_t luma_default = 1 << luma_denom;
  const int32_t chroma_default = 1 << chroma_denom;
  for (uint32_t i = 0; i < count; ++i) {
    const auto idx = static_cast<int>(i);
    l.luma_weight[i] = luma_default;
    l.luma_offset[i] = 0;
    l.luma_weight_flag[i] = detail::read_flag_at(b, n.luma_weight_flag, idx);
    if (l.luma_weight_flag[i]) {
      l.luma_weight[i] = detail::read_se_at(b, n.luma_weight, idx);
      check_weight(b, n.luma_weight, l.luma_weight[i]);
      l.luma_offset[i] = detail::read_se_at(b, n.luma_offset, idx);
      check_weight(b, n.luma_offset, l.luma_offset[i]);
    }
    for (unsigned j = 0; j < 2; ++j) {
      l.chroma_weight[i][j] = chroma_default;
      l.chroma_offset[i][j] = 0;
    }
    if (!chroma) {
      continue;
    }
    l.chroma_weight_flag[i] = detail::read_flag_at(b, n.chroma_weight_flag, idx);
    if (!l.chroma_weight_flag[i]) {
      continue;
    }
    for (unsigned j = 0; j < 2; ++j) {
      const auto jdx = static_cast<int>(j);
      b.begin_array_element(n.chroma_weight, idx);
      l.chroma_weight[i][j] = detail::read_se_at(b, n.chroma_weight, jdx);
      check_weight(b, n.chroma_weight, l.chroma_weight[i][j]);
      b.end_array_element();
      b.begin_array_element(n.chroma_offset, idx);
      l.chroma_offset[i][j] = detail::read_se_at(b, n.chroma_offset, jdx);
      check_weight(b, n.chroma_offset, l.chroma_offset[i][j]);
      b.end_array_element();
    }
  }
}

}  // namespace

PredWeightTable parse_pred_weight_table(BitReader& br, SyntaxBuilder& b, uint32_t slice_type,
                                        uint32_t chroma_array_type, uint32_t num_ref_idx_l0_active_minus1,
                                        uint32_t num_ref_idx_l1_active_minus1) {
  PredWeightTable t;
  const auto start = br.mark();
  auto scope = b.scoped("pred_weight_table", "H.264 7.3.3.2");
  // §7.3.1 rule 3: the loop bounds are checked before any loop is entered.
  detail::require_range(br, "num_ref_idx_l0_active_minus1", num_ref_idx_l0_active_minus1, 0, kMaxRefIdx - 1);
  detail::require_range(br, "num_ref_idx_l1_active_minus1", num_ref_idx_l1_active_minus1, 0, kMaxRefIdx - 1);
  t.luma_log2_weight_denom = b.read_ue("luma_log2_weight_denom");
  detail::require_range(br, "luma_log2_weight_denom", t.luma_log2_weight_denom, 0, 7);
  const bool chroma = chroma_array_type != 0;
  if (chroma) {
    t.chroma_log2_weight_denom = b.read_ue("chroma_log2_weight_denom");
    detail::require_range(br, "chroma_log2_weight_denom", t.chroma_log2_weight_denom, 0, 7);
  }
  read_list(b, kL0, num_ref_idx_l0_active_minus1 + 1, t.luma_log2_weight_denom, t.chroma_log2_weight_denom, chroma,
            t.l0);
  if (slice_type % 5 == kSliceB) {
    read_list(b, kL1, num_ref_idx_l1_active_minus1 + 1, t.luma_log2_weight_denom, t.chroma_log2_weight_denom, chroma,
              t.l1);
  }
  t.span = br.span_since(start);
  return t;
}

}  // namespace ovt::syntax::h264
