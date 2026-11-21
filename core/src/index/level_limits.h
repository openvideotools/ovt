// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Level-limit conformance checks of the index (chapter 7 §7.9 `SYN-CONF-0001`/`SYN-CONF-0002`, WI-4.109): the
// picture-size and DPB-size limits of H.264 Table A-1 / A.3.1, H.265 Table A.8 / A.4.2 and AV1 Annex A (A.3), as
// constexpr tables and pure functions over the typed parameter sets. The StreamIndex builder (WI-1.121) calls
// them once per activated SPS / sequence header and attaches the Warnings to the frame. Module-internal, not
// installed.

#ifndef OVT_SRC_INDEX_LEVEL_LIMITS_H
#define OVT_SRC_INDEX_LEVEL_LIMITS_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::index {

/// A Diagnostic raised by a conformance check; `code` is `SYN-CONF-0001` or `SYN-CONF-0002`.
struct ConformanceDiagnostic {
  syntax::Severity severity = syntax::Severity::Warning;
  std::string code;
  std::string message;
};

/// H.264 Table A-1 MaxFS in macroblocks for a level name as `Sps::level_name` spells it ("1b", "1.0" … "6.2");
/// 0 for an unlisted name. MaxLumaPs of the level is MaxFS * 256.
[[nodiscard]] uint32_t h264_max_fs(std::string_view level_name) noexcept;

/// H.265 Table A.8 MaxLumaPs for general_level_idc (30 × level number); 0 for a level without a row.
[[nodiscard]] uint32_t h265_max_luma_ps(uint32_t general_level_idc) noexcept;

/// H.265 §A.4.2 MaxDpbSize for a picture of `pic_size_in_samples_y` luma samples at `max_luma_ps`;
/// `max_dpb_pic_buf` is 6, or 7 when sps_curr_pic_ref_enabled_flag is set.
[[nodiscard]] uint32_t h265_max_dpb_size(uint64_t pic_size_in_samples_y, uint32_t max_luma_ps,
                                         uint32_t max_dpb_pic_buf) noexcept;

/// AV1 Annex A.3 limits of one seq_level_idx. `valid` is false for reserved indices and for 31 (no limits).
struct Av1LevelLimits {
  bool valid = false;
  uint32_t max_pic_size = 0;  // MaxPicSize, luma samples
  uint32_t max_h_size = 0;
  uint32_t max_v_size = 0;
};
[[nodiscard]] Av1LevelLimits av1_level_limits(uint32_t seq_level_idx) noexcept;

/// SYN-CONF-0001 when the frame size exceeds MaxFS * 256 of the signalled level, SYN-CONF-0002 when
/// max_num_ref_frames (or the VUI max_dec_frame_buffering) exceeds MaxDpbFrames (§A.3.1 items h, i). An
/// unlisted level_idc is not checked.
[[nodiscard]] std::vector<ConformanceDiagnostic> check_levels(const syntax::h264::Sps& sps);

/// SYN-CONF-0001 for PicSizeInSamplesY > MaxLumaPs (§A.4.1 / Table A.8), SYN-CONF-0002 for
/// sps_max_dec_pic_buffering_minus1[HighestTid] + 1 > MaxDpbSize (§A.4.2).
[[nodiscard]] std::vector<ConformanceDiagnostic> check_levels(const syntax::h265::Sps& sps);

/// SYN-CONF-0001 for max_frame_width * max_frame_height > MaxPicSize of any operating point's level (AV1 A.3);
/// the message names the level of the first operating point that fails. Reserved levels and 31 are not checked.
/// AV1 has no level DPB limit, so SYN-CONF-0002 is never raised.
[[nodiscard]] std::vector<ConformanceDiagnostic> check_levels(const syntax::av1::SequenceHeader& seq);

}  // namespace ovt::index

#endif  // OVT_SRC_INDEX_LEVEL_LIMITS_H
