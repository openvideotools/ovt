// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Level-limit conformance checks (WI-4.109); see level_limits.h.
// SPEC-ASSUMPTION(#591): picture size and DPB size only; the bit-rate limits (MaxBR, MaxCPB) are not checked.

#include "level_limits.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include <fmt/format.h>

namespace ovt::index {

namespace {

using syntax::Severity;

struct H264Level {
  std::string_view name;
  uint32_t max_fs;  // macroblocks
};

// H.264 Table A-1, MaxFS column.
constexpr std::array<H264Level, 19> kH264Levels{{
    {"1b", 99},     {"1.0", 99},    {"1.1", 396},   {"1.2", 396},    {"1.3", 396},    {"2.0", 396},  {"2.1", 792},
    {"2.2", 1620},  {"3.0", 1620},  {"3.1", 3600},  {"3.2", 5120},   {"4.0", 8192},   {"4.1", 8192}, {"4.2", 8704},
    {"5.0", 22080}, {"5.1", 36864}, {"5.2", 36864}, {"6.0", 139264}, {"6.1", 139264},
}};

struct H265Level {
  uint32_t general_level_idc;
  uint32_t max_luma_ps;
};

// H.265 Table A.8, MaxLumaPs column (level 6.2 shares the 6.0 row; general_level_idc = 30 * level).
constexpr std::array<H265Level, 13> kH265Levels{{
    {30, 36864},
    {60, 122880},
    {63, 245760},
    {90, 552960},
    {93, 983040},
    {120, 2228224},
    {123, 2228224},
    {150, 8912896},
    {153, 8912896},
    {156, 8912896},
    {180, 35651584},
    {183, 35651584},
    {186, 35651584},
}};

// AV1 Annex A.3 Table A.3 (seq_level_idx 0..23); indices 2, 3, 6, 7, 10, 11 are reserved. Levels 7.x repeat 6.x.
constexpr Av1LevelLimits kAv1Level20{true, 147456, 2048, 1152};
constexpr Av1LevelLimits kAv1Level21{true, 278784, 2816, 1584};
constexpr Av1LevelLimits kAv1Level30{true, 665856, 4352, 2448};
constexpr Av1LevelLimits kAv1Level31{true, 1065024, 5504, 3096};
constexpr Av1LevelLimits kAv1Level40{true, 2359296, 6144, 3456};
constexpr Av1LevelLimits kAv1Level50{true, 8912896, 8192, 4352};
constexpr Av1LevelLimits kAv1Level60{true, 35651584, 16384, 8704};

std::string h265_level_name(uint32_t idc) {
  return fmt::format("{}.{}", idc / 30U, (idc % 30U) / 3U);
}

}  // namespace

uint32_t h264_max_fs(std::string_view level_name) noexcept {
  const auto it = std::find_if(kH264Levels.begin(), kH264Levels.end(),
                               [level_name](const H264Level& l) { return l.name == level_name; });
  return it == kH264Levels.end() ? 0U : it->max_fs;
}

uint32_t h265_max_luma_ps(uint32_t general_level_idc) noexcept {
  const auto it = std::find_if(kH265Levels.begin(), kH265Levels.end(), [general_level_idc](const H265Level& l) {
    return l.general_level_idc == general_level_idc;
  });
  return it == kH265Levels.end() ? 0U : it->max_luma_ps;
}

uint32_t h265_max_dpb_size(uint64_t pic_size_in_samples_y, uint32_t max_luma_ps, uint32_t max_dpb_pic_buf) noexcept {
  if (pic_size_in_samples_y <= (max_luma_ps >> 2U)) {
    return std::min(4U * max_dpb_pic_buf, 16U);
  }
  if (pic_size_in_samples_y <= (max_luma_ps >> 1U)) {
    return std::min(2U * max_dpb_pic_buf, 16U);
  }
  if (pic_size_in_samples_y <= ((3U * static_cast<uint64_t>(max_luma_ps)) >> 2U)) {
    return std::min((4U * max_dpb_pic_buf) / 3U, 16U);
  }
  return max_dpb_pic_buf;
}

Av1LevelLimits av1_level_limits(uint32_t seq_level_idx) noexcept {
  switch (seq_level_idx) {
    case 0:
      return kAv1Level20;
    case 1:
      return kAv1Level21;
    case 4:
      return kAv1Level30;
    case 5:
      return kAv1Level31;
    case 8:
    case 9:
      return kAv1Level40;
    case 12:
    case 13:
    case 14:
    case 15:
      return kAv1Level50;
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
      return kAv1Level60;
    default:
      return {};
  }
}

std::vector<ConformanceDiagnostic> check_levels(const syntax::h264::Sps& sps) {
  std::vector<ConformanceDiagnostic> out;
  const uint32_t max_fs = h264_max_fs(sps.level_name);
  if (max_fs == 0U) {
    return out;
  }
  const uint64_t frame_size = static_cast<uint64_t>(sps.PicWidthInMbs) * sps.FrameHeightInMbs;
  if (frame_size > max_fs) {
    out.push_back(
        {Severity::Warning, "SYN-CONF-0001", fmt::format("picture size exceeds level {} MaxLumaPs", sps.level_name)});
  }
  // A.3.1 items h and i: max_num_ref_frames and max_dec_frame_buffering are bounded by MaxDpbFrames.
  if (sps.MaxDpbMbs != 0U) {
    const uint32_t dpb_frames = sps.MaxDpbFrames;
    const bool vui_limit = sps.vui_parameters_present_flag && sps.vui.bitstream_restriction_flag;
    if (sps.max_num_ref_frames > dpb_frames || (vui_limit && sps.vui.max_dec_frame_buffering > dpb_frames)) {
      out.push_back({Severity::Warning, "SYN-CONF-0002", "DPB size exceeds level limit"});
    }
  }
  return out;
}

std::vector<ConformanceDiagnostic> check_levels(const syntax::h265::Sps& sps) {
  std::vector<ConformanceDiagnostic> out;
  const uint32_t idc = sps.profile_tier_level.general_level_idc;
  const uint32_t max_luma_ps = h265_max_luma_ps(idc);
  if (max_luma_ps == 0U) {
    return out;
  }
  if (sps.PicSizeInSamplesY > max_luma_ps) {
    out.push_back({Severity::Warning, "SYN-CONF-0001",
                   fmt::format("picture size exceeds level {} MaxLumaPs", h265_level_name(idc))});
  }
  const uint32_t pic_buf = sps.scc_extension.sps_curr_pic_ref_enabled_flag ? 7U : 6U;
  const uint32_t max_dpb = h265_max_dpb_size(sps.PicSizeInSamplesY, max_luma_ps, pic_buf);
  if (static_cast<uint64_t>(sps.MaxDpbSizeMinus1) + 1U > max_dpb) {
    out.push_back({Severity::Warning, "SYN-CONF-0002", "DPB size exceeds level limit"});
  }
  return out;
}

std::vector<ConformanceDiagnostic> check_levels(const syntax::av1::SequenceHeader& seq) {
  std::vector<ConformanceDiagnostic> out;
  const uint64_t pic_size = static_cast<uint64_t>(seq.max_frame_width) * seq.max_frame_height;
  for (const auto& op : seq.operating_points) {
    const Av1LevelLimits limits = av1_level_limits(op.seq_level_idx);
    if (limits.valid && (pic_size > limits.max_pic_size || seq.max_frame_width > limits.max_h_size ||
                         seq.max_frame_height > limits.max_v_size)) {
      out.push_back(
          {Severity::Warning, "SYN-CONF-0001",
           fmt::format("picture size exceeds level {} MaxLumaPs", syntax::av1::level_name(op.seq_level_idx))});
      break;  // one finding per sequence header; the first failing operating point names the level
    }
  }
  return out;
}

}  // namespace ovt::index
