// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 frame-boundary detection (chapter 7 §7.3.9, H.264 §7.4.1.2.3 and §7.4.1.2.4), WI-1.111.

#include "h264_index.h"

namespace ovt::index::h264 {

namespace {

// Rule 10 (§7.4.1.2.3): AUD, SEI, SPS, PPS, subset SPS and the reserved types 16..18 may only precede the first VCL
// NAL unit of a primary coded picture. The prefix NAL (14) is in the same H.264 list but immediately precedes every
// base-view slice of an MVC / SVC stream, so it is decided by the slice that follows it.
// SPEC-ASSUMPTION(#568): rule 10 does not fire on a prefix NAL; the following slice applies rules 1..9.
bool rule10_type(uint32_t t) noexcept {
  return t == 6 || t == 7 || t == 8 || t == 9 || t == 15 || (t >= 16 && t <= 18);
}

constexpr const char* kFirstMbZero = "SYN-H264-0060";

}  // namespace

BoundaryDecision FrameBoundary::on_non_vcl(const syntax::h264::NalHeader& nal) {
  BoundaryDecision d;
  const uint32_t t = nal.nal_unit_type;
  if (rule10_type(t)) {
    d.role = UnitRole::Preceding;
    // After an end of sequence / stream the picture is closed, but its access unit is still being collected: the
    // first rule-10 unit starts the next one, so a parameter set there is not taken as part of the old picture.
    if ((picture_open_ || after_end_of_seq_) && !au_started_) {
      d.starts_access_unit = true;
      au_started_ = true;
      after_end_of_seq_ = false;
    }
    rule10_since_vcl_ = picture_open_;
    return d;
  }
  if (t == 14) {
    d.role = UnitRole::Prefix;
    return d;
  }
  d.role = UnitRole::Following;
  if (t == 10 || t == 11) {  // end of sequence / stream: the next slice starts a picture and pairs with nothing
    picture_open_ = false;
    rule10_since_vcl_ = false;
    au_started_ = false;
    after_end_of_seq_ = true;
    last_ = {};
  }
  return d;
}

BoundaryDecision FrameBoundary::on_slice_without_header(const syntax::h264::NalHeader& nal) const {
  BoundaryDecision d;
  d.role = (nal.nal_unit_type == 20 || nal.nal_unit_type == 21) ? UnitRole::ExtensionSlice : UnitRole::PartitionBC;
  d.picture_index = picture_open_ ? next_index_ - 1 : -1;
  return d;
}

BoundaryDecision FrameBoundary::on_slice(const syntax::h264::NalHeader& nal, const syntax::h264::SliceHeader& sh,
                                         uint32_t pic_order_cnt_type) {
  if (nal.nal_unit_type == 20 || nal.nal_unit_type == 21) {
    return on_slice_without_header(nal);
  }
  BoundaryDecision d;
  Key cur;
  cur.frame_num = sh.frame_num;
  cur.pic_parameter_set_id = sh.pic_parameter_set_id;
  cur.field_pic_flag = sh.field_pic_flag;
  cur.bottom_field_flag = sh.bottom_field_flag;
  cur.reference = nal.nal_ref_idc != 0;
  cur.pic_order_cnt_type = pic_order_cnt_type;
  cur.pic_order_cnt_lsb = sh.pic_order_cnt_lsb;
  cur.delta_pic_order_cnt_bottom = sh.delta_pic_order_cnt_bottom;
  cur.delta_pic_order_cnt = sh.delta_pic_order_cnt;
  cur.idr = sh.IdrPicFlag;
  cur.idr_pic_id = sh.idr_pic_id;

  if (picture_open_) {
    // A redundant coded picture belongs to its primary coded picture and is never compared (§7.4.1.2.4).
    if (sh.redundant_pic_cnt > 0) {
      d.role = UnitRole::RedundantSlice;
      d.picture_index = next_index_ - 1;
      return d;
    }
    const Key& p = prev_;
    auto rule = [&d](bool fired, uint8_t n) {
      if (fired) {
        d.rules.push_back(n);
      }
    };
    rule(cur.frame_num != p.frame_num, 1);
    rule(cur.pic_parameter_set_id != p.pic_parameter_set_id, 2);
    rule(cur.field_pic_flag != p.field_pic_flag, 3);
    rule(cur.field_pic_flag && p.field_pic_flag && cur.bottom_field_flag != p.bottom_field_flag, 4);
    rule(cur.reference != p.reference, 5);
    rule(cur.pic_order_cnt_type == 0 && p.pic_order_cnt_type == 0 &&
             (cur.pic_order_cnt_lsb != p.pic_order_cnt_lsb ||
              cur.delta_pic_order_cnt_bottom != p.delta_pic_order_cnt_bottom),
         6);
    rule(cur.pic_order_cnt_type == 1 && p.pic_order_cnt_type == 1 &&
             (cur.delta_pic_order_cnt[0] != p.delta_pic_order_cnt[0] ||
              cur.delta_pic_order_cnt[1] != p.delta_pic_order_cnt[1]),
         7);
    rule(cur.idr != p.idr, 8);
    rule(cur.idr && p.idr && cur.idr_pic_id != p.idr_pic_id, 9);
    rule(rule10_since_vcl_, 10);
    if (d.rules.empty()) {
      d.role = UnitRole::NextSlice;
      d.picture_index = next_index_ - 1;
      if (sh.first_mb_in_slice == 0) {
        d.diagnostics.push_back(
            {Severity::Info, kFirstMbZero, "slice with first_mb_in_slice 0 does not start a new picture"});
      }
      return d;
    }
  }

  // A new primary coded picture.
  d.role = UnitRole::FirstSlice;
  d.starts_access_unit = !au_started_;
  d.picture_index = next_index_++;
  d.structure = sh.structure;
  // Complementary field pair (§7.4.1.2.4 note, H.264 3.29 / 3.30): consecutive fields of opposite parity with the
  // same frame_num, both reference or both non-reference (as JM pairs them), the first not already paired, and the
  // second not an IDR unless the first was.
  if (cur.field_pic_flag && last_.valid && last_.field && !last_.paired && last_.bottom != cur.bottom_field_flag &&
      last_.frame_num == cur.frame_num && last_.reference == cur.reference && (!cur.idr || last_.idr)) {
    d.second_field_of = last_.index;
  }
  last_.valid = true;
  last_.index = d.picture_index;
  last_.field = cur.field_pic_flag;
  last_.bottom = cur.bottom_field_flag;
  last_.reference = cur.reference;
  last_.idr = cur.idr;
  last_.paired = d.second_field_of >= 0;
  last_.frame_num = cur.frame_num;

  prev_ = cur;
  picture_open_ = true;
  rule10_since_vcl_ = false;
  au_started_ = false;
  after_end_of_seq_ = false;
  return d;
}

}  // namespace ovt::index::h264
