// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// HDR10+ dynamic metadata, SMPTE ST 2094-40 / CTA-861-H Annex (chapter 7 §7.3.11, WI-1.120).

#include "syntax/common/user_data_registry.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "ovt/cxx/bitreader.hpp"

namespace ovt::syntax {

namespace {

constexpr unsigned kMaxGridDimension = 25;  // ST 2094-40: 25 x 25 maximum

Hdr10PlusGrid read_grid(BitReader& br) {
  Hdr10PlusGrid g;
  const uint64_t at = br.bit_pos();
  g.num_rows = static_cast<uint8_t>(br.u(5));
  g.num_cols = static_cast<uint8_t>(br.u(5));
  if (g.num_rows > kMaxGridDimension || g.num_cols > kMaxGridDimension) {
    throw ParseError("SYN-GEN-0003", "ST 2094-40 peak luminance grid larger than 25x25", at);
  }
  g.actual_peak_luminance.reserve(static_cast<size_t>(g.num_rows) * g.num_cols);
  for (unsigned i = 0; i < static_cast<unsigned>(g.num_rows) * g.num_cols; ++i) {
    g.actual_peak_luminance.push_back(static_cast<uint8_t>(br.u(4)));
  }
  return g;
}

}  // namespace

Hdr10Plus parse_hdr10plus(BitReader& br) {
  Hdr10Plus m;
  m.application_identifier = static_cast<uint8_t>(br.u(8));
  m.application_version = static_cast<uint8_t>(br.u(8));
  const uint64_t windows_at = br.bit_pos();
  m.num_windows = static_cast<uint8_t>(br.u(2));
  if (m.num_windows < 1 || m.num_windows > 3) {
    throw ParseError("SYN-GEN-0003", "ST 2094-40 num_windows must be 1..3", windows_at);
  }
  for (unsigned i = 1; i < m.num_windows; ++i) {
    Hdr10PlusWindow w{};
    w.window_upper_left_corner_x = static_cast<uint16_t>(br.u(16));
    w.window_upper_left_corner_y = static_cast<uint16_t>(br.u(16));
    w.window_lower_right_corner_x = static_cast<uint16_t>(br.u(16));
    w.window_lower_right_corner_y = static_cast<uint16_t>(br.u(16));
    w.center_of_ellipse_x = static_cast<uint16_t>(br.u(16));
    w.center_of_ellipse_y = static_cast<uint16_t>(br.u(16));
    w.rotation_angle = static_cast<uint8_t>(br.u(8));
    w.semimajor_axis_internal_ellipse = static_cast<uint16_t>(br.u(16));
    w.semimajor_axis_external_ellipse = static_cast<uint16_t>(br.u(16));
    w.semiminor_axis_external_ellipse = static_cast<uint16_t>(br.u(16));
    w.overlap_process_option = static_cast<uint8_t>(br.u(1));
    m.windows.push_back(w);
  }
  m.targeted_system_display_maximum_luminance = static_cast<uint32_t>(br.u(27));
  if (br.u(1) != 0) {  // targeted_system_display_actual_peak_luminance_flag
    m.targeted_system_display_actual_peak_luminance = read_grid(br);
  }
  m.processing.resize(m.num_windows);
  for (Hdr10PlusProcessingWindow& w : m.processing) {
    for (uint32_t& s : w.maxscl) {
      s = static_cast<uint32_t>(br.u(17));
    }
    w.average_maxrgb = static_cast<uint32_t>(br.u(17));
    const auto num_percentiles = static_cast<unsigned>(br.u(4));
    for (unsigned i = 0; i < num_percentiles; ++i) {
      Hdr10PlusPercentile p{};
      p.percentage = static_cast<uint8_t>(br.u(7));
      p.percentile = static_cast<uint32_t>(br.u(17));
      w.distribution_maxrgb.push_back(p);
    }
    w.fraction_bright_pixels = static_cast<uint16_t>(br.u(10));
  }
  if (br.u(1) != 0) {  // mastering_display_actual_peak_luminance_flag
    m.mastering_display_actual_peak_luminance = read_grid(br);
  }
  for (Hdr10PlusProcessingWindow& w : m.processing) {
    if (br.u(1) != 0) {  // tone_mapping_flag
      Hdr10PlusToneMapping t;
      t.knee_point_x = static_cast<uint16_t>(br.u(12));
      t.knee_point_y = static_cast<uint16_t>(br.u(12));
      const auto anchors = static_cast<unsigned>(br.u(4));
      for (unsigned i = 0; i < anchors; ++i) {
        t.bezier_curve_anchors.push_back(static_cast<uint16_t>(br.u(10)));
      }
      w.tone_mapping = std::move(t);
    }
    if (br.u(1) != 0) {  // color_saturation_mapping_flag
      w.color_saturation_weight = static_cast<uint8_t>(br.u(6));
    }
  }
  return m;
}

}  // namespace ovt::syntax
