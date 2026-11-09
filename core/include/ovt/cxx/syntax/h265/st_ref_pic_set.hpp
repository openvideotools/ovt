// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 st_ref_pic_set(stRpsIdx) (H.265 §7.3.7, §7.4.8): typed struct and parser with the §7.3.10.3 derivation
// (chapter 7 §7.3.5 row 9). Implemented by WI-1.112 (step 4) because the SPS calls it; WI-1.113 reuses it for the
// slice segment header. C++ helper header (D22): semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H265_ST_REF_PIC_SET_HPP
#define OVT_CXX_SYNTAX_H265_ST_REF_PIC_SET_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// Typed st_ref_pic_set(stRpsIdx) (§7.3.7). Arrays hold up to 16 entries (sps_max_dec_pic_buffering_minus1 <= 15,
/// A.4.2); NumDeltaPocs <= 16.
struct StRefPicSet {
  static constexpr unsigned kMaxPics = 16;
  uint32_t stRpsIdx = 0;
  bool inter_ref_pic_set_prediction_flag = false;
  uint32_t delta_idx_minus1 = 0;
  bool delta_rps_sign = false;
  uint32_t abs_delta_rps_minus1 = 0;
  std::array<bool, kMaxPics + 1> used_by_curr_pic_flag{};  // j <= NumDeltaPocs[RefRpsIdx]
  std::array<bool, kMaxPics + 1> use_delta_flag{};         // inferred 1 when absent
  uint32_t num_negative_pics = 0;
  uint32_t num_positive_pics = 0;
  std::array<uint32_t, kMaxPics> delta_poc_s0_minus1{};
  std::array<bool, kMaxPics> used_by_curr_pic_s0_flag{};
  std::array<uint32_t, kMaxPics> delta_poc_s1_minus1{};
  std::array<bool, kMaxPics> used_by_curr_pic_s1_flag{};
  // derived (§7.4.8, equations 7-59..7-62)
  uint32_t RefRpsIdx = 0;
  int32_t deltaRps = 0;
  uint32_t NumNegativePics = 0;
  uint32_t NumPositivePics = 0;
  uint32_t NumDeltaPocs = 0;
  std::array<int32_t, kMaxPics> DeltaPocS0{};
  std::array<bool, kMaxPics> UsedByCurrPicS0{};
  std::array<int32_t, kMaxPics> DeltaPocS1{};
  std::array<bool, kMaxPics> UsedByCurrPicS1{};
  BitSpan span{0, 0};
};

/// The derived text of §7.3.5 row 9, e.g. "S0: -1(used) -2(used) -4 | S1: +1(used)".
[[nodiscard]] std::string st_ref_pic_set_summary(const StRefPicSet& s);

/// Parses st_ref_pic_set(stRpsIdx) into the node `st_ref_pic_set[stRpsIdx]` (an array element) and derives the
/// delta-POC lists. `sps_sets` are the sets 0..num_short_term_ref_pic_sets-1 already parsed from the SPS (the
/// prediction source; only the first stRpsIdx entries are read). `max_dec_pic_buffering_minus1` bounds
/// num_negative_pics / num_positive_pics (Diagnostic Error SYN-H265-0007 beyond it; ParseError beyond 16).
/// abs_delta_rps_minus1 and delta_poc_s0/s1_minus1 beyond 2^15 - 1 (§7.4.8) are ParseErrors too, so the derived
/// DeltaPoc arithmetic cannot overflow; a predicted set that would exceed 16 entries is truncated with a Diagnostic.
StRefPicSet parse_st_ref_pic_set(BitReader& br, SyntaxBuilder& b, uint32_t st_rps_idx,
                                 uint32_t num_short_term_ref_pic_sets, std::span<const StRefPicSet> sps_sets,
                                 uint32_t max_dec_pic_buffering_minus1);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_ST_REF_PIC_SET_HPP
