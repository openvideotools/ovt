// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.109: level-limit conformance checks (index/level_limits.h). The parameter sets are filled field by field
// (the checks read only the fields set here), with the derived values computed as the parsers compute them.

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "index/level_limits.h"

using namespace ovt::index;
namespace h264 = ovt::syntax::h264;
namespace h265 = ovt::syntax::h265;
namespace av1 = ovt::syntax::av1;

namespace {

bool has(const std::vector<ConformanceDiagnostic>& d, const std::string& code) {
  for (const auto& x : d) {
    if (x.code == code) {
      return true;
    }
  }
  return false;
}

h265::Sps hevc(uint32_t level_idc, uint32_t w, uint32_t h, uint32_t dpb_minus1) {
  h265::Sps s;
  s.profile_tier_level.general_level_idc = static_cast<uint8_t>(level_idc);
  s.pic_width_in_luma_samples = w;
  s.pic_height_in_luma_samples = h;
  s.PicSizeInSamplesY = static_cast<uint64_t>(w) * h;
  s.MaxDpbSizeMinus1 = dpb_minus1;
  return s;
}

h264::Sps avc(uint32_t profile, uint32_t level_idc, bool cs3, uint32_t w_mbs, uint32_t h_mbs, uint32_t refs) {
  h264::Sps s;
  s.profile_idc = profile;
  s.level_idc = level_idc;
  s.constraint_set3_flag = cs3;
  s.PicWidthInMbs = w_mbs;
  s.FrameHeightInMbs = h_mbs;
  s.max_num_ref_frames = refs;
  s.MaxDpbMbs = h264::max_dpb_mbs(profile, level_idc, cs3, s.level_name);
  const uint64_t frame = static_cast<uint64_t>(w_mbs) * h_mbs;
  s.MaxDpbFrames = s.MaxDpbMbs == 0 ? 0 : static_cast<uint32_t>(std::min<uint64_t>(s.MaxDpbMbs / frame, 16));
  return s;
}

av1::SequenceHeader seq(uint32_t w, uint32_t h, std::vector<uint8_t> levels) {
  av1::SequenceHeader s;
  s.max_frame_width = w;
  s.max_frame_height = h;
  for (const uint8_t l : levels) {
    av1::OperatingPoint op;
    op.seq_level_idx = l;
    s.operating_points.push_back(op);
  }
  return s;
}

}  // namespace

TEST_CASE("AT2: an H.265 SPS declaring level 3.0 with 1080p raises SYN-CONF-0001", "[index][levels][fast]") {
  const auto d = check_levels(hevc(90, 1920, 1080, 3));
  REQUIRE(has(d, "SYN-CONF-0001"));
  CHECK(d.front().severity == ovt::syntax::Severity::Warning);
  CHECK(d.front().message == "picture size exceeds level 3.0 MaxLumaPs");

  // 1080p fits level 4.0 (MaxLumaPs 2 228 224) and level 4.1; 4K needs level 5.
  CHECK(check_levels(hevc(120, 1920, 1080, 3)).empty());
  CHECK(check_levels(hevc(123, 1920, 1080, 3)).empty());
  CHECK(has(check_levels(hevc(123, 3840, 2160, 3)), "SYN-CONF-0001"));
  CHECK(check_levels(hevc(150, 3840, 2160, 3)).empty());
  // The limit itself is allowed: 36864 luma samples at level 1.
  CHECK(check_levels(hevc(30, 256, 144, 0)).empty());
  CHECK(has(check_levels(hevc(30, 256, 145, 0)), "SYN-CONF-0001"));
  // A level without a Table A.8 row is not checked.
  CHECK(check_levels(hevc(0, 8192, 4320, 15)).empty());
  CHECK(check_levels(hevc(255, 8192, 4320, 15)).empty());
}

TEST_CASE("H.265 MaxDpbSize of A.4.2 and SYN-CONF-0002", "[index][levels][fast]") {
  CHECK(h265_max_dpb_size(1000, 36864, 6) == 16);               // <= MaxLumaPs/4: min(4*6, 16)
  CHECK(h265_max_dpb_size(36864 / 2, 36864, 6) == 12);          // <= MaxLumaPs/2: 2*6
  CHECK(h265_max_dpb_size((36864U * 3U) / 4U, 36864, 6) == 8);  // <= 3/4: 4*6/3
  CHECK(h265_max_dpb_size(36864, 36864, 6) == 6);
  CHECK(h265_max_dpb_size(36864, 36864, 7) == 7);

  // 1080p at level 4.1: PicSize 2 073 600 > 3/4 * 2 228 224 = 1 671 168, so MaxDpbSize 6.
  CHECK(check_levels(hevc(123, 1920, 1080, 5)).empty());  // 6 pictures
  const auto d = check_levels(hevc(123, 1920, 1080, 6));  // 7 pictures
  REQUIRE(d.size() == 1);
  CHECK(d[0].code == "SYN-CONF-0002");
  CHECK(d[0].message == "DPB size exceeds level limit");
  // 720p at level 4.1 is <= 3/4 MaxLumaPs (921 600 <= 1 671 168), <= 1/2 (1 114 112): MaxDpbSize 12.
  CHECK(check_levels(hevc(123, 1280, 720, 11)).empty());
  CHECK(has(check_levels(hevc(123, 1280, 720, 12)), "SYN-CONF-0002"));
  // Current-picture referencing (SCC) raises maxDpbPicBuf to 7.
  h265::Sps scc = hevc(123, 1920, 1080, 6);
  scc.scc_extension.sps_curr_pic_ref_enabled_flag = true;
  CHECK(check_levels(scc).empty());
}

TEST_CASE("H.264 Table A-1 MaxFS and the DPB limit", "[index][levels][fast]") {
  CHECK(h264_max_fs("3.0") == 1620);
  CHECK(h264_max_fs("4.1") == 8192);
  CHECK(h264_max_fs("1b") == 99);
  CHECK(h264_max_fs("6.2") == 139264);
  CHECK(h264_max_fs("9.9") == 0);
  CHECK(h264_max_fs("") == 0);

  // 1920x1088 = 120 x 68 MBs = 8160 at level 4.0 (MaxFS 8192, MaxDpbMbs 32768): four reference frames fit.
  CHECK(check_levels(avc(100, 40, false, 120, 68, 4)).empty());
  // Level 3.0 allows 1620 MBs: 720x576 (45 x 36 = 1620) fits, 1280x720 (80 x 45 = 3600) does not.
  CHECK(check_levels(avc(77, 30, false, 45, 36, 4)).empty());
  const auto big = check_levels(avc(77, 30, false, 80, 45, 1));
  REQUIRE(has(big, "SYN-CONF-0001"));
  CHECK(big.front().message == "picture size exceeds level 3.0 MaxLumaPs");

  // 5 reference frames at level 4.0 / 1080p: MaxDpbFrames = 32768 / 8160 = 4.
  const auto dpb = check_levels(avc(100, 40, false, 120, 68, 5));
  REQUIRE(dpb.size() == 1);
  CHECK(dpb[0].code == "SYN-CONF-0002");
  // The VUI max_dec_frame_buffering is bounded too, but only when the bitstream restriction is present.
  h264::Sps vui = avc(100, 40, false, 120, 68, 2);
  vui.vui_parameters_present_flag = true;
  vui.vui.max_dec_frame_buffering = 5;
  CHECK(check_levels(vui).empty());
  vui.vui.bitstream_restriction_flag = true;
  CHECK(has(check_levels(vui), "SYN-CONF-0002"));
  // Level 1b via level_idc 11 + constraint_set3 on Main: MaxFS 99.
  CHECK(has(check_levels(avc(77, 11, true, 20, 10, 1)), "SYN-CONF-0001"));
  // An unlisted level_idc is not checked.
  CHECK(check_levels(avc(100, 99, false, 500, 500, 16)).empty());
}

TEST_CASE("AV1 Annex A MaxPicSize, MaxHSize and MaxVSize per operating point", "[index][levels][fast]") {
  CHECK(check_levels(seq(1920, 1080, {8})).empty());   // level 4.0
  CHECK(check_levels(seq(3840, 2160, {12})).empty());  // level 5.0
  const auto d = check_levels(seq(3840, 2160, {8}));   // 4K at level 4.0
  REQUIRE(d.size() == 1);
  CHECK(d[0].code == "SYN-CONF-0001");
  CHECK(d[0].message == "picture size exceeds level 4.0 MaxLumaPs");
  // MaxHSize alone can fail: 2200 x 100 is small but wider than the 2048 of level 2.0.
  CHECK(has(check_levels(seq(2200, 100, {0})), "SYN-CONF-0001"));
  // Two operating points: the first failing one names the level, once.
  const auto two = check_levels(seq(1920, 1080, {8, 0, 1}));
  REQUIRE(two.size() == 1);
  CHECK(two[0].message == "picture size exceeds level 2.0 MaxLumaPs");
  // Level 31 (no limits) and reserved indices are not checked.
  CHECK(check_levels(seq(65536, 65536, {31})).empty());
  CHECK(check_levels(seq(65536, 65536, {2, 3, 6, 7, 10, 11, 20, 21, 22, 23, 24})).empty());
  CHECK(av1_level_limits(19).max_pic_size == 35651584);
  CHECK_FALSE(av1_level_limits(20).valid);  // 7.x: no limits in Annex A, never checked
  CHECK(av1_level_limits(9).valid);
  CHECK_FALSE(av1_level_limits(31).valid);
}
