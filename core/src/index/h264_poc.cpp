// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 picture order count (chapter 7 §7.3.10.1, H.264 §8.2.1.1–§8.2.1.3), WI-1.111.

#include <algorithm>

#include "h264_index.h"

namespace ovt::index::h264 {

namespace {

// FrameNumOffset grows by MaxFrameNum (at most 2^16) per frame_num wrap and PicOrderCntMsb by MaxPicOrderCntLsb per
// picture; a crafted stream could walk either until int32 overflows, so both stay within ±2^29 and every order count
// within ±2^30 (PocResult::poc_clamped reports it).
constexpr int64_t kStateLimit = int64_t{1} << 29;
// Bound of picOrderCntCycleCnt * ExpectedDeltaPerPicOrderCntCycle before the additions (each below 2^40).
constexpr int64_t kProductLimit = int64_t{1} << 40;

int64_t clamp(int64_t v, int64_t limit, bool& clamped) {
  if (v > limit || v < -limit) {
    clamped = true;
    return v > 0 ? limit : -limit;
  }
  return v;
}

// The picture's shape, read once from the first slice header.
struct Shape {
  bool field = false;
  bool bottom = false;
  bool reference = false;
  bool idr = false;
};

// TopFieldOrderCnt / BottomFieldOrderCnt before clamping; the one a field picture lacks is unused.
struct Counts {
  int64_t top = 0;
  int64_t bottom = 0;
};

// §8.2.1.1, from prevPicOrderCntMsb / prevPicOrderCntLsb. Returns the counts and sets `msb`.
Counts type0(const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps, const Shape& s, int64_t prev_msb,
             int64_t prev_lsb, int64_t& msb, bool& clamped) {
  const int64_t max_lsb = sps.MaxPicOrderCntLsb;
  const int64_t lsb = sh.pic_order_cnt_lsb;
  msb = prev_msb;
  if (lsb < prev_lsb && (prev_lsb - lsb) >= max_lsb / 2) {
    msb = prev_msb + max_lsb;
  } else if (lsb > prev_lsb && (lsb - prev_lsb) > max_lsb / 2) {
    msb = prev_msb - max_lsb;
  }
  msb = clamp(msb, kStateLimit, clamped);
  Counts c;
  c.top = msb + lsb;
  c.bottom = s.field ? msb + lsb : c.top + sh.delta_pic_order_cnt_bottom;
  return c;
}

// §8.2.1.2: expectedPicOrderCnt from FrameNumOffset.
int64_t expected_type1(const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps, const Shape& s,
                       int64_t frame_num_offset, bool& clamped) {
  const int64_t cycle = sps.num_ref_frames_in_pic_order_cnt_cycle;
  int64_t abs_frame_num = cycle != 0 ? frame_num_offset + sh.frame_num : 0;
  if (!s.reference && abs_frame_num > 0) {
    abs_frame_num -= 1;
  }
  int64_t expected = 0;
  if (abs_frame_num > 0) {
    const int64_t cycle_cnt = (abs_frame_num - 1) / cycle;
    const int64_t in_cycle = (abs_frame_num - 1) % cycle;
    const int64_t delta = sps.ExpectedDeltaPerPicOrderCntCycle;
    if (cycle_cnt != 0 && (delta > kProductLimit / cycle_cnt || delta < -kProductLimit / cycle_cnt)) {
      expected = delta > 0 ? kProductLimit : -kProductLimit;
      clamped = true;
    } else {
      expected = cycle_cnt * delta;
    }
    const auto n = std::min<size_t>(static_cast<size_t>(in_cycle) + 1, sps.offset_for_ref_frame.size());
    for (size_t i = 0; i < n; ++i) {
      expected += sps.offset_for_ref_frame[i];
    }
  }
  if (!s.reference) {
    expected += sps.offset_for_non_ref_pic;
  }
  return expected;
}

Counts type1(const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps, const Shape& s,
             int64_t frame_num_offset, bool& clamped) {
  const int64_t expected = expected_type1(sh, sps, s, frame_num_offset, clamped);
  Counts c;
  c.top = expected + sh.delta_pic_order_cnt[0];
  if (!s.field) {
    c.bottom = c.top + sps.offset_for_top_to_bottom_field + sh.delta_pic_order_cnt[1];
  } else {
    c.bottom = expected + sps.offset_for_top_to_bottom_field + sh.delta_pic_order_cnt[0];
  }
  return c;
}

// §8.2.1.3
Counts type2(const syntax::h264::SliceHeader& sh, const Shape& s, int64_t frame_num_offset) {
  int64_t temp = 0;
  if (!s.idr) {
    temp = 2 * (frame_num_offset + sh.frame_num) - (s.reference ? 0 : 1);
  }
  return {temp, temp};
}

// Clamps the counts into `r`, derives PicOrderCnt(CurrPic) (eq. 8-1) and the mmco5-adjusted counts.
void finish(const Counts& c, const Shape& s, PocResult& r, bool& clamped) {
  r.top_field_order_cnt = r.has_top ? static_cast<int32_t>(clamp(c.top, kPocLimit, clamped)) : 0;
  r.bottom_field_order_cnt = r.has_bottom ? static_cast<int32_t>(clamp(c.bottom, kPocLimit, clamped)) : 0;
  // eq. 8-1
  if (!s.field) {
    r.poc = std::min(r.top_field_order_cnt, r.bottom_field_order_cnt);
  } else {
    r.poc = s.bottom ? r.bottom_field_order_cnt : r.top_field_order_cnt;
  }
  // §8.2.1: after the decoding of a picture with memory_management_control_operation 5, tempPicOrderCnt =
  // PicOrderCnt(CurrPic) is subtracted from its TopFieldOrderCnt / BottomFieldOrderCnt.
  r.top_after_mmco5 = r.top_field_order_cnt;
  r.bottom_after_mmco5 = r.bottom_field_order_cnt;
  if (r.mmco5) {
    r.top_after_mmco5 = r.has_top ? r.top_field_order_cnt - r.poc : 0;
    r.bottom_after_mmco5 = r.has_bottom ? r.bottom_field_order_cnt - r.poc : 0;
  }
}

}  // namespace

PocResult PocState::step(const syntax::h264::NalHeader& nal, const syntax::h264::SliceHeader& sh,
                         const syntax::h264::Sps& sps) {
  const Shape s{sh.field_pic_flag, sh.field_pic_flag && sh.bottom_field_flag, nal.nal_ref_idc != 0, sh.IdrPicFlag};
  PocResult r;
  r.structure = !s.field   ? PictureStructure::Frame
                : s.bottom ? PictureStructure::BottomField
                           : PictureStructure::TopField;
  r.has_top = !s.bottom;
  r.has_bottom = !s.field || s.bottom;
  r.mmco5 = sh.has_dec_ref_pic_marking && sh.dec_ref_pic_marking.has_mmco5;
  bool clamped = false;

  if (s.idr) {  // §8.2.1: an IDR picture resets the state of all three types
    prev_msb_ = 0;
    prev_lsb_ = 0;
    prev_frame_num_offset_ = 0;
    prev_frame_num_ = 0;
  }

  Counts c;
  if (sps.pic_order_cnt_type == 0) {
    r.prev_pic_order_cnt_msb = static_cast<int32_t>(prev_msb_);
    r.prev_pic_order_cnt_lsb = static_cast<int32_t>(prev_lsb_);
    int64_t msb = 0;
    c = type0(sh, sps, s, prev_msb_, prev_lsb_, msb, clamped);
    r.pic_order_cnt_msb = static_cast<int32_t>(msb);
  } else {
    // FrameNumOffset, shared by types 1 and 2 (§8.2.1.2).
    r.prev_frame_num_offset = static_cast<int32_t>(prev_frame_num_offset_);
    int64_t frame_num_offset = 0;
    if (!s.idr) {
      frame_num_offset = prev_frame_num_offset_ + (prev_frame_num_ > sh.frame_num ? int64_t{sps.MaxFrameNum} : 0);
    }
    frame_num_offset = clamp(frame_num_offset, kStateLimit, clamped);
    r.frame_num_offset = static_cast<int32_t>(frame_num_offset);
    c = sps.pic_order_cnt_type == 1 ? type1(sh, sps, s, frame_num_offset, clamped) : type2(sh, s, frame_num_offset);
  }

  finish(c, s, r, clamped);
  r.poc_clamped = clamped;
  advance(r, sh, sps, s.reference, s.bottom);
  return r;
}

void PocState::advance(const PocResult& r, const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps,
                       bool reference, bool bottom_field) {
  if (sps.pic_order_cnt_type != 0) {
    // §8.2.1.2: prevFrameNumOffset is 0 after a picture with mmco5, whose frame_num is then inferred to be 0.
    prev_frame_num_offset_ = r.mmco5 ? 0 : r.frame_num_offset;
    prev_frame_num_ = r.mmco5 ? 0 : sh.frame_num;
    return;
  }
  // Type 0: only reference pictures; a non-reference picture's lsb is never remembered.
  if (!reference) {
    return;
  }
  if (r.mmco5) {  // §8.2.1.1: prevPicOrderCntMsb = 0, prevPicOrderCntLsb = 0 or the adjusted TopFieldOrderCnt
    prev_msb_ = 0;
    prev_lsb_ = bottom_field ? 0 : r.top_after_mmco5;
  } else {
    prev_msb_ = r.pic_order_cnt_msb;
    prev_lsb_ = sh.pic_order_cnt_lsb;
  }
}

}  // namespace ovt::index::h264
