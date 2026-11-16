// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 buffering_period() (D.1.2) and pic_timing() (D.1.3): chapter 7 §7.3.4 rows 8 and 9, WI-1.110. The HRD
// field lengths come from the hrd_parameters() of the SPS the caller selected (D.2.2).

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/h264/sei.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "syntax/h264/h264_internal.h"
#include "syntax/h264/sei_internal.h"

namespace ovt::syntax::h264::detail {

namespace {

// Table D-1.
std::string_view pic_struct_name(uint32_t pic_struct) noexcept {
  switch (pic_struct) {
    case 0:
      return "frame";
    case 1:
      return "top field";
    case 2:
      return "bottom field";
    case 3:
      return "top field, bottom field, in that order";
    case 4:
      return "bottom field, top field, in that order";
    case 5:
      return "top field, bottom field, top field repeated, in that order";
    case 6:
      return "bottom field, top field, bottom field repeated, in that order";
    case 7:
      return "frame doubling";
    case 8:
      return "frame tripling";
    default:
      return "reserved";
  }
}

// Table D-1 NumClockTS; 0 for a reserved pic_struct.
uint32_t num_clock_ts(uint32_t pic_struct) noexcept {
  switch (pic_struct) {
    case 0:
    case 1:
    case 2:
      return 1;
    case 3:
    case 4:
    case 7:
      return 2;
    case 5:
    case 6:
    case 8:
      return 3;
    default:
      return 0;
  }
}

// Reads `count` (initial_cpb_removal_delay, initial_cpb_removal_delay_offset) pairs of one HRD.
void read_delay_pairs(SeiPayloadReader& r, const HrdParameters& hrd, std::vector<uint32_t>& delay,
                      std::vector<uint32_t>& offset) {
  const unsigned bits = hrd.initial_cpb_removal_delay_length_minus1 + 1;
  for (uint32_t i = 0; i <= hrd.cpb_cnt_minus1; ++i) {
    const ArrayElementScope element(r.builder(), "initial_cpb_removal_delay", static_cast<int>(i));
    delay.push_back(static_cast<uint32_t>(r.u("initial_cpb_removal_delay", bits)));
    r.builder().set_descriptor_text("u(v)");
    offset.push_back(static_cast<uint32_t>(r.u("initial_cpb_removal_delay_offset", bits)));
    r.builder().set_descriptor_text("u(v)");
  }
}

ClockTimestamp parse_clock_timestamp(SeiPayloadReader& r, int index, unsigned time_offset_length) {
  ClockTimestamp c;
  SyntaxBuilder& b = r.builder();
  const ArrayElementScope element(b, "clock_timestamp", index);
  c.clock_timestamp_flag = r.flag("clock_timestamp_flag");
  if (c.clock_timestamp_flag) {
    c.ct_type = static_cast<uint32_t>(r.u("ct_type", 2));
    c.nuit_field_based_flag = r.flag("nuit_field_based_flag");
    c.counting_type = static_cast<uint32_t>(r.u("counting_type", 5));
    c.full_timestamp_flag = r.flag("full_timestamp_flag");
    c.discontinuity_flag = r.flag("discontinuity_flag");
    c.cnt_dropped_flag = r.flag("cnt_dropped_flag");
    c.n_frames = static_cast<uint32_t>(r.u("n_frames", 8));
    if (c.full_timestamp_flag) {
      c.seconds_flag = c.minutes_flag = c.hours_flag = true;
      c.seconds_value = static_cast<uint32_t>(r.u("seconds_value", 6));
      c.minutes_value = static_cast<uint32_t>(r.u("minutes_value", 6));
      c.hours_value = static_cast<uint32_t>(r.u("hours_value", 5));
    } else {
      c.seconds_flag = r.flag("seconds_flag");
      if (c.seconds_flag) {
        c.seconds_value = static_cast<uint32_t>(r.u("seconds_value", 6));
        c.minutes_flag = r.flag("minutes_flag");
        if (c.minutes_flag) {
          c.minutes_value = static_cast<uint32_t>(r.u("minutes_value", 6));
          c.hours_flag = r.flag("hours_flag");
          if (c.hours_flag) {
            c.hours_value = static_cast<uint32_t>(r.u("hours_value", 5));
          }
        }
      }
    }
    if (time_offset_length > 0) {
      c.time_offset = static_cast<int32_t>(r.i("time_offset", time_offset_length));
      b.set_descriptor_text("i(v)");
    }
    c.text = fmt::format("{:02}:{:02}:{:02}:{:02}", c.hours_value, c.minutes_value, c.seconds_value, c.n_frames);
    b.derived("clockTimestamp", c.text);
  }
  return c;
}

}  // namespace

BufferingPeriod parse_buffering_period(SeiPayloadReader& r, const Sps& sps) {
  BufferingPeriod bp;
  bp.seq_parameter_set_id = r.ue("seq_parameter_set_id");
  require_range(r.reader(), "seq_parameter_set_id", bp.seq_parameter_set_id, 0, 31);
  const VuiParameters& vui = sps.vui;
  if (sps.vui_parameters_present_flag && vui.nal_hrd_parameters_present_flag) {
    read_delay_pairs(r, vui.nal_hrd, bp.nal_initial_cpb_removal_delay, bp.nal_initial_cpb_removal_delay_offset);
  }
  if (sps.vui_parameters_present_flag && vui.vcl_hrd_parameters_present_flag) {
    read_delay_pairs(r, vui.vcl_hrd, bp.vcl_initial_cpb_removal_delay, bp.vcl_initial_cpb_removal_delay_offset);
  }
  return bp;
}

PicTiming parse_pic_timing(SeiPayloadReader& r, const Sps* sps) {
  PicTiming pt;
  pt.sps_known = sps != nullptr;
  if (sps == nullptr) {
    return pt;
  }
  const VuiParameters& vui = sps->vui;
  const bool has_vui = sps->vui_parameters_present_flag;
  // CpbDpbDelaysPresentFlag (E.2.1); the field lengths come from the NAL HRD when present, else the VCL HRD.
  const HrdParameters* hrd = nullptr;
  if (has_vui && vui.nal_hrd_parameters_present_flag) {
    hrd = &vui.nal_hrd;
  } else if (has_vui && vui.vcl_hrd_parameters_present_flag) {
    hrd = &vui.vcl_hrd;
  }
  pt.cpb_dpb_delays_present = hrd != nullptr;
  if (hrd != nullptr) {
    pt.cpb_removal_delay = static_cast<uint32_t>(r.u("cpb_removal_delay", hrd->cpb_removal_delay_length_minus1 + 1));
    r.builder().set_descriptor_text("u(v)");
    pt.dpb_output_delay = static_cast<uint32_t>(r.u("dpb_output_delay", hrd->dpb_output_delay_length_minus1 + 1));
    r.builder().set_descriptor_text("u(v)");
  }
  pt.pic_struct_present = has_vui && vui.pic_struct_present_flag;
  if (pt.pic_struct_present) {
    pt.pic_struct = static_cast<uint32_t>(r.u("pic_struct", 4));
    pt.pic_struct_name = pic_struct_name(pt.pic_struct);
    r.builder().set_enum(pt.pic_struct_name);
    pt.NumClockTS = num_clock_ts(pt.pic_struct);
    if (pt.NumClockTS == 0) {
      warn_reserved(r.builder(), "pic_struct", pt.pic_struct, "H.264 Table D-1");
      return pt;
    }
    r.builder().derived("NumClockTS", pt.NumClockTS);
    const unsigned time_offset_length = hrd != nullptr ? hrd->time_offset_length : 24;
    for (uint32_t i = 0; i < pt.NumClockTS; ++i) {
      pt.clock_timestamps.push_back(parse_clock_timestamp(r, static_cast<int>(i), time_offset_length));
    }
  }
  return pt;
}

}  // namespace ovt::syntax::h264::detail
