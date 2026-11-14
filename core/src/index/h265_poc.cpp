// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 picture order count (chapter 7 §7.3.10.2, H.265 §8.3.1), WI-1.115.

#include "h265_index.h"

namespace ovt::index::h265 {

namespace {
constexpr int64_t kPocLimit = int64_t{1} << 29;
}  // namespace

PocResult PocState::step(const syntax::h265::NalHeader& nal, const syntax::h265::SliceSegmentHeader& sh,
                         const syntax::h265::Sps& sps) {
  PocResult r;
  r.is_rap = nal.is_irap;
  r.is_rasl = nal.is_rasl;
  const bool first_or_after_eos = next_no_rasl_;
  next_no_rasl_ = false;
  if (nal.is_irap) {
    r.no_rasl_output_flag = nal.is_idr || nal.is_bla || first_or_after_eos || (nal.is_cra && handle_cra_as_bla_);
    irap_no_rasl_output_ = r.no_rasl_output_flag;
  }
  r.rasl_skipped = nal.is_rasl && irap_no_rasl_output_;

  const int64_t max_lsb = sps.MaxPicOrderCntLsb;
  const int64_t lsb = sh.slice_pic_order_cnt_lsb;  // 0 for IDR (inferred)
  if (nal.is_irap && r.no_rasl_output_flag) {
    r.poc_msb = 0;
  } else {
    // prevPicOrderCntLsb / Msb from prevTid0Pic (floor modulo, so negative POCs keep the spec's & semantics).
    const int64_t prev_lsb = ((prev_tid0_poc_ % max_lsb) + max_lsb) % max_lsb;
    const int64_t prev_msb = prev_tid0_poc_ - prev_lsb;
    int64_t msb = prev_msb;
    if (lsb < prev_lsb && (prev_lsb - lsb) >= max_lsb / 2) {
      msb = prev_msb + max_lsb;
    } else if (lsb > prev_lsb && (lsb - prev_lsb) > max_lsb / 2) {
      msb = prev_msb - max_lsb;
    }
    // A crafted stream can walk the MSB by ±MaxPicOrderCntLsb per picture; keep every later POC sum inside int32.
    if (msb > kPocLimit || msb < -kPocLimit) {
      msb = msb > 0 ? kPocLimit : -kPocLimit;
      r.poc_clamped = true;
    }
    r.poc_msb = static_cast<int32_t>(msb);
  }
  r.poc = static_cast<int32_t>(static_cast<int64_t>(r.poc_msb) + lsb);

  // prevTid0Pic: TemporalId 0, not RASL, RADL or a sub-layer non-reference picture.
  if (nal.TemporalId == 0 && !nal.is_rasl && !nal.is_radl && !nal.is_sub_layer_non_ref) {
    prev_tid0_poc_ = r.poc;
    r.updates_prev_tid0 = true;
  }
  return r;
}

}  // namespace ovt::index::h265
