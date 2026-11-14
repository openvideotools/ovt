// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 pred_weight_table() parser and the §7.4.7.3 weight derivation (H.265 §7.3.6.3; chapter 7 §7.3.5 row 12;
// WI-1.113).

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

struct ListNames {
  const char* luma_weight_flag;
  const char* chroma_weight_flag;
  const char* delta_luma_weight;
  const char* luma_offset;
  const char* delta_chroma_weight;
  const char* delta_chroma_offset;
};

constexpr ListNames kL0{"luma_weight_l0_flag", "chroma_weight_l0_flag",  "delta_luma_weight_l0",
                        "luma_offset_l0",      "delta_chroma_weight_l0", "delta_chroma_offset_l0"};
constexpr ListNames kL1{"luma_weight_l1_flag", "chroma_weight_l1_flag",  "delta_luma_weight_l1",
                        "luma_offset_l1",      "delta_chroma_weight_l1", "delta_chroma_offset_l1"};

struct Ranges {
  uint32_t luma_log2_weight_denom;
  uint32_t ChromaLog2WeightDenom;
  int64_t half_range_y;  // WpOffsetHalfRangeY
  int64_t half_range_c;  // WpOffsetHalfRangeC
  bool chroma;           // ChromaArrayType != 0
};

// One reference list: the flags first (luma for all i, then chroma for all i), then the per-entry weights, in the
// order of the §7.3.6.3 table. Every value is hard-bounded (§7.4.7.3) before it enters the derivation, so the
// int32_t arithmetic below cannot overflow (|weight| <= 255, |offset| <= 4 * 2^15).
void read_list(BitReader& br, SyntaxBuilder& b, const ListNames& n, uint32_t count, const Ranges& r,
               PredWeightTable::List& l) {
  for (uint32_t i = 0; i < count; ++i) {
    l.luma_weight_flag[i] = detail::read_flag_at(b, n.luma_weight_flag, static_cast<int>(i));
  }
  if (r.chroma) {
    for (uint32_t i = 0; i < count; ++i) {
      l.chroma_weight_flag[i] = detail::read_flag_at(b, n.chroma_weight_flag, static_cast<int>(i));
    }
  }
  const int32_t luma_default = 1 << r.luma_log2_weight_denom;
  const int32_t chroma_default = 1 << r.ChromaLog2WeightDenom;
  const auto half_c = static_cast<int32_t>(r.half_range_c);
  for (uint32_t i = 0; i < count; ++i) {
    l.LumaWeight[i] = luma_default;
    if (l.luma_weight_flag[i]) {
      l.delta_luma_weight[i] = detail::read_se_at(b, n.delta_luma_weight, static_cast<int>(i));
      detail::require_range(br, n.delta_luma_weight, l.delta_luma_weight[i], -128, 127);
      l.luma_offset[i] = detail::read_se_at(b, n.luma_offset, static_cast<int>(i));
      detail::require_range(br, n.luma_offset, l.luma_offset[i], -r.half_range_y, r.half_range_y - 1);
      l.LumaWeight[i] = luma_default + l.delta_luma_weight[i];
    }
    for (unsigned j = 0; j < 2; ++j) {
      l.ChromaWeight[i][j] = chroma_default;
      l.ChromaOffset[i][j] = 0;
    }
    if (!l.chroma_weight_flag[i]) {
      continue;
    }
    for (unsigned j = 0; j < 2; ++j) {
      b.begin_array_element(n.delta_chroma_weight, static_cast<int>(i));
      l.delta_chroma_weight[i][j] = detail::read_se_at(b, n.delta_chroma_weight, static_cast<int>(j));
      b.end_array_element();
      detail::require_range(br, n.delta_chroma_weight, l.delta_chroma_weight[i][j], -128, 127);
      b.begin_array_element(n.delta_chroma_offset, static_cast<int>(i));
      l.delta_chroma_offset[i][j] = detail::read_se_at(b, n.delta_chroma_offset, static_cast<int>(j));
      b.end_array_element();
      detail::require_range(br, n.delta_chroma_offset, l.delta_chroma_offset[i][j], -4 * r.half_range_c,
                            4 * r.half_range_c - 1);
      const int32_t weight = chroma_default + l.delta_chroma_weight[i][j];
      l.ChromaWeight[i][j] = weight;
      // 7-56
      l.ChromaOffset[i][j] = std::clamp(
          half_c - ((half_c * weight) >> r.ChromaLog2WeightDenom) + l.delta_chroma_offset[i][j], -half_c, half_c - 1);
    }
  }
}

uint32_t sum_weight_flags(const PredWeightTable::List& l, uint32_t count) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < count; ++i) {
    sum += (l.luma_weight_flag[i] ? 1U : 0U) + (l.chroma_weight_flag[i] ? 2U : 0U);
  }
  return sum;
}

}  // namespace

PredWeightTable parse_pred_weight_table(BitReader& br, SyntaxBuilder& b, const Sps& sps, const Pps& pps,
                                        uint32_t slice_type, uint32_t num_ref_idx_l0_active_minus1,
                                        uint32_t num_ref_idx_l1_active_minus1) {
  PredWeightTable t;
  const uint64_t start = br.bit_pos();
  auto s = b.scoped("pred_weight_table", "H.265 7.3.6.3");
  detail::require_range(br, "num_ref_idx_l0_active_minus1", num_ref_idx_l0_active_minus1, 0, kMaxRefIdx - 1);
  detail::require_range(br, "num_ref_idx_l1_active_minus1", num_ref_idx_l1_active_minus1, 0, kMaxRefIdx - 1);
  if (pps.scc_extension.pps_curr_pic_ref_enabled_flag) {
    b.diag(Severity::Warning, "SYN-H265-0040", "RefPicList0 unknown at parse time; SCC current-picture entries ignored",
           "H.265 7.3.6.3");
  }
  t.luma_log2_weight_denom = b.read_ue("luma_log2_weight_denom");
  detail::require_range(br, "luma_log2_weight_denom", t.luma_log2_weight_denom, 0, 7);
  const bool chroma = sps.ChromaArrayType != 0;
  t.ChromaLog2WeightDenom = t.luma_log2_weight_denom;
  if (chroma) {
    t.delta_chroma_log2_weight_denom = b.read_se("delta_chroma_log2_weight_denom");
    const int64_t denom = int64_t{t.luma_log2_weight_denom} + t.delta_chroma_log2_weight_denom;
    detail::require_range(br, "ChromaLog2WeightDenom", denom, 0, 7);
    t.ChromaLog2WeightDenom = static_cast<uint32_t>(denom);
    b.derived("ChromaLog2WeightDenom", t.ChromaLog2WeightDenom);
  }
  const Ranges r{t.luma_log2_weight_denom, t.ChromaLog2WeightDenom, sps.WpOffsetHalfRangeY, sps.WpOffsetHalfRangeC,
                 chroma};
  read_list(br, b, kL0, num_ref_idx_l0_active_minus1 + 1, r, t.l0);
  uint32_t sum = sum_weight_flags(t.l0, num_ref_idx_l0_active_minus1 + 1);
  if (slice_type == kSliceB) {
    read_list(br, b, kL1, num_ref_idx_l1_active_minus1 + 1, r, t.l1);
    sum += sum_weight_flags(t.l1, num_ref_idx_l1_active_minus1 + 1);
  }
  // §7.4.7.3: sumWeightL0Flags (P) or sumWeightL0Flags + sumWeightL1Flags (B) shall be <= 24.
  detail::check_range(b, slice_type == kSliceB ? "sumWeightL0Flags + sumWeightL1Flags" : "sumWeightL0Flags", sum, 0, 24,
                      "H.265 7.4.7.3");
  t.span = br.span_since(start);
  return t;
}

}  // namespace ovt::syntax::h265
