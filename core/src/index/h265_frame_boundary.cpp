// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 frame-boundary detection (chapter 7 §7.3.9, H.265 §7.4.2.4.4), WI-1.115.

#include "h265_index.h"

namespace ovt::index::h265 {

namespace {

// §7.4.2.4.4: after the last VCL NAL of an access unit, the first of these starts a new access unit.
bool starts_access_unit_type(uint8_t t) noexcept {
  return t == syntax::h265::kAudNut || t == syntax::h265::kVpsNut || t == syntax::h265::kSpsNut ||
         t == syntax::h265::kPpsNut || t == syntax::h265::kPrefixSeiNut || (t >= 41 && t <= 44) || (t >= 48 && t <= 55);
}

}  // namespace

BoundaryDecision FrameBoundary::on_unit(const syntax::h265::NalHeader& nal, bool first_slice_segment_in_pic_flag) {
  BoundaryDecision d;
  const unsigned layer = nal.nuh_layer_id & 63U;
  if (nal.is_vcl) {
    if (first_slice_segment_in_pic_flag) {
      d.role = UnitRole::FirstSlice;
      if (layer == 0) {
        d.starts_access_unit = vcl_seen_in_au_;  // back-to-back access units with no non-VCL unit between
        vcl_seen_in_au_ = true;
      }
      picture_open_[layer] = true;
    } else if (picture_open_[layer]) {
      d.role = UnitRole::NextSlice;
    } else {
      d.role = UnitRole::OrphanSlice;
      d.diagnostics.push_back({syntax::Severity::Error, "SYN-H265-0060", "slice segment without first slice segment"});
      picture_open_[layer] = true;  // the synthetic frame takes the following slice segments
      if (layer == 0) {
        vcl_seen_in_au_ = true;
      }
    }
    return d;
  }
  if (starts_access_unit_type(nal.nal_unit_type)) {
    d.role = UnitRole::Preceding;
    if (vcl_seen_in_au_) {
      d.starts_access_unit = true;
      vcl_seen_in_au_ = false;
      picture_open_.fill(false);
    }
    return d;
  }
  d.role = UnitRole::Following;
  if (nal.nal_unit_type == syntax::h265::kEosNut || nal.nal_unit_type == syntax::h265::kEobNut) {
    d.is_eos = nal.nal_unit_type == syntax::h265::kEosNut;
    vcl_seen_in_au_ = false;
    picture_open_.fill(false);
  }
  return d;
}

}  // namespace ovt::index::h265
