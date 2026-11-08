// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 st_ref_pic_set() parser and the §7.4.8 derivation (chapter 7 §7.3.5 row 9, §7.3.10.3; WI-1.112 step 4).

#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <span>
#include <string>

#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

// Equations 7-61 and 7-62 as written in §7.3.10.3.
void derive_predicted(StRefPicSet& s, const StRefPicSet& ref) {
  const auto nn = static_cast<int>(ref.NumNegativePics);
  const auto np = static_cast<int>(ref.NumPositivePics);
  const auto nd = static_cast<int>(ref.NumDeltaPocs);
  auto used = [&s](int j) { return s.used_by_curr_pic_flag[static_cast<size_t>(j)]; };
  auto use_delta = [&s](int j) { return s.use_delta_flag[static_cast<size_t>(j)]; };
  // 7-61: S0
  uint32_t i = 0;
  for (int j = np - 1; j >= 0; --j) {
    const int32_t d_poc = ref.DeltaPocS1[static_cast<size_t>(j)] + s.deltaRps;
    if (d_poc < 0 && use_delta(nn + j) && i < StRefPicSet::kMaxPics) {
      s.DeltaPocS0[i] = d_poc;
      s.UsedByCurrPicS0[i++] = used(nn + j);
    }
  }
  if (s.deltaRps < 0 && use_delta(nd) && i < StRefPicSet::kMaxPics) {
    s.DeltaPocS0[i] = s.deltaRps;
    s.UsedByCurrPicS0[i++] = used(nd);
  }
  for (int j = 0; j < nn; ++j) {
    const int32_t d_poc = ref.DeltaPocS0[static_cast<size_t>(j)] + s.deltaRps;
    if (d_poc < 0 && use_delta(j) && i < StRefPicSet::kMaxPics) {
      s.DeltaPocS0[i] = d_poc;
      s.UsedByCurrPicS0[i++] = used(j);
    }
  }
  s.NumNegativePics = i;
  // 7-62: S1
  i = 0;
  for (int j = nn - 1; j >= 0; --j) {
    const int32_t d_poc = ref.DeltaPocS0[static_cast<size_t>(j)] + s.deltaRps;
    if (d_poc > 0 && use_delta(j) && i < StRefPicSet::kMaxPics) {
      s.DeltaPocS1[i] = d_poc;
      s.UsedByCurrPicS1[i++] = used(j);
    }
  }
  if (s.deltaRps > 0 && use_delta(nd) && i < StRefPicSet::kMaxPics) {
    s.DeltaPocS1[i] = s.deltaRps;
    s.UsedByCurrPicS1[i++] = used(nd);
  }
  for (int j = 0; j < np; ++j) {
    const int32_t d_poc = ref.DeltaPocS1[static_cast<size_t>(j)] + s.deltaRps;
    if (d_poc > 0 && use_delta(nn + j) && i < StRefPicSet::kMaxPics) {
      s.DeltaPocS1[i] = d_poc;
      s.UsedByCurrPicS1[i++] = used(nn + j);
    }
  }
  s.NumPositivePics = i;
}

void parse_predicted(BitReader& br, SyntaxBuilder& b, StRefPicSet& s, uint32_t num_short_term_ref_pic_sets,
                     std::span<const StRefPicSet> sps_sets) {
  if (s.stRpsIdx == num_short_term_ref_pic_sets) {  // called from the slice segment header
    s.delta_idx_minus1 = b.read_ue("delta_idx_minus1");
    detail::require_range(br, "delta_idx_minus1", s.delta_idx_minus1, 0, int64_t{s.stRpsIdx} - 1);
  }
  s.delta_rps_sign = b.read_flag("delta_rps_sign");
  s.abs_delta_rps_minus1 = b.read_ue("abs_delta_rps_minus1");
  detail::check_range(b, "abs_delta_rps_minus1", s.abs_delta_rps_minus1, 0, (1 << 15) - 1, "H.265 7.4.8");
  s.RefRpsIdx = s.stRpsIdx - (s.delta_idx_minus1 + 1);
  s.deltaRps = (1 - 2 * static_cast<int32_t>(s.delta_rps_sign)) * static_cast<int32_t>(s.abs_delta_rps_minus1 + 1);
  if (s.RefRpsIdx >= sps_sets.size()) {
    throw ParseError{"SYN-H265-0007",
                     fmt::format("RefRpsIdx out of range (RefRpsIdx = {}, allowed [0,{}])", s.RefRpsIdx,
                                 sps_sets.empty() ? 0 : sps_sets.size() - 1),
                     br.bit_pos()};
  }
  const StRefPicSet& ref = sps_sets[s.RefRpsIdx];
  b.derived("RefRpsIdx", s.RefRpsIdx);
  b.derived("deltaRps", s.deltaRps);
  for (uint32_t j = 0; j <= ref.NumDeltaPocs; ++j) {
    s.used_by_curr_pic_flag[j] = detail::read_flag_at(b, "used_by_curr_pic_flag", static_cast<int>(j));
    s.use_delta_flag[j] = true;
    if (!s.used_by_curr_pic_flag[j]) {
      s.use_delta_flag[j] = detail::read_flag_at(b, "use_delta_flag", static_cast<int>(j));
    }
  }
  derive_predicted(s, ref);
}

void parse_explicit(BitReader& br, SyntaxBuilder& b, StRefPicSet& s, uint32_t max_dec_pic_buffering_minus1) {
  s.num_negative_pics = b.read_ue("num_negative_pics");
  detail::require_range(br, "num_negative_pics", s.num_negative_pics, 0, StRefPicSet::kMaxPics);
  detail::check_range(b, "num_negative_pics", s.num_negative_pics, 0, max_dec_pic_buffering_minus1, "H.265 7.4.8");
  s.num_positive_pics = b.read_ue("num_positive_pics");
  detail::require_range(br, "num_positive_pics", s.num_positive_pics, 0, StRefPicSet::kMaxPics - s.num_negative_pics);
  detail::check_range(b, "num_positive_pics", s.num_positive_pics, 0,
                      int64_t{max_dec_pic_buffering_minus1} - s.num_negative_pics, "H.265 7.4.8");
  int32_t poc = 0;
  for (uint32_t i = 0; i < s.num_negative_pics; ++i) {
    s.delta_poc_s0_minus1[i] = detail::read_ue_at(b, "delta_poc_s0_minus1", static_cast<int>(i));
    detail::check_range(b, "delta_poc_s0_minus1", s.delta_poc_s0_minus1[i], 0, (1 << 15) - 1, "H.265 7.4.8");
    s.used_by_curr_pic_s0_flag[i] = detail::read_flag_at(b, "used_by_curr_pic_s0_flag", static_cast<int>(i));
    poc -= static_cast<int32_t>(s.delta_poc_s0_minus1[i] + 1);  // 7-59
    s.DeltaPocS0[i] = poc;
    s.UsedByCurrPicS0[i] = s.used_by_curr_pic_s0_flag[i];
  }
  poc = 0;
  for (uint32_t i = 0; i < s.num_positive_pics; ++i) {
    s.delta_poc_s1_minus1[i] = detail::read_ue_at(b, "delta_poc_s1_minus1", static_cast<int>(i));
    detail::check_range(b, "delta_poc_s1_minus1", s.delta_poc_s1_minus1[i], 0, (1 << 15) - 1, "H.265 7.4.8");
    s.used_by_curr_pic_s1_flag[i] = detail::read_flag_at(b, "used_by_curr_pic_s1_flag", static_cast<int>(i));
    poc += static_cast<int32_t>(s.delta_poc_s1_minus1[i] + 1);  // 7-60
    s.DeltaPocS1[i] = poc;
    s.UsedByCurrPicS1[i] = s.used_by_curr_pic_s1_flag[i];
  }
  s.NumNegativePics = s.num_negative_pics;
  s.NumPositivePics = s.num_positive_pics;
}

}  // namespace

std::string st_ref_pic_set_summary(const StRefPicSet& s) {
  std::string out = "S0:";
  for (uint32_t i = 0; i < s.NumNegativePics; ++i) {
    fmt::format_to(std::back_inserter(out), " {}{}", s.DeltaPocS0[i], s.UsedByCurrPicS0[i] ? "(used)" : "");
  }
  out += " | S1:";
  for (uint32_t i = 0; i < s.NumPositivePics; ++i) {
    fmt::format_to(std::back_inserter(out), " +{}{}", s.DeltaPocS1[i], s.UsedByCurrPicS1[i] ? "(used)" : "");
  }
  return out;
}

StRefPicSet parse_st_ref_pic_set(BitReader& br, SyntaxBuilder& b, uint32_t st_rps_idx,
                                 uint32_t num_short_term_ref_pic_sets, std::span<const StRefPicSet> sps_sets,
                                 uint32_t max_dec_pic_buffering_minus1) {
  StRefPicSet s;
  s.stRpsIdx = st_rps_idx;
  const uint64_t start = br.bit_pos();
  b.begin_array_element("st_ref_pic_set", static_cast<int>(st_rps_idx), "H.265 7.3.7");
  if (st_rps_idx != 0) {
    s.inter_ref_pic_set_prediction_flag = b.read_flag("inter_ref_pic_set_prediction_flag");
  }
  if (s.inter_ref_pic_set_prediction_flag) {
    parse_predicted(br, b, s, num_short_term_ref_pic_sets, sps_sets);
  } else {
    parse_explicit(br, b, s, max_dec_pic_buffering_minus1);
  }
  s.NumDeltaPocs = s.NumNegativePics + s.NumPositivePics;
  b.derived("NumDeltaPocs", s.NumDeltaPocs);
  b.derived("DeltaPoc", std::string_view{st_ref_pic_set_summary(s)});
  b.end_array_element();
  s.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return s;
}

}  // namespace ovt::syntax::h265
