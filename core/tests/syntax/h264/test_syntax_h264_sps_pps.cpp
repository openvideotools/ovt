// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.108a: H.264 NAL header and SPS (VUI, HRD, scaling lists) parsers (chapter 7 §7.3.4 rows 1–5).
// WI-1.108b adds the PPS tests (row 6) to this file.
//
// Fixtures: the SPS/PPS NAL units of streams encoded with x264 (FFmpeg 6.1 libx264, 2026-11-08) from an
// lavfi testsrc2 source; the expected values are hard-coded from `ffmpeg -i <stream> -c copy -bsf:v
// trace_headers -f null -` of the same files and cited per test. The acceptance-test streams of the WI
// (`syn_h264_gop60_1080p`) are read from the corpus cache when present ([needs-corpus], skipped otherwise).
// The error paths use hand-written bit strings.

#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "syntax/common/syntax_node_buffer.h"
#include "test_support/corpus.h"

namespace {

using ovt::BitReader;
using ovt::ParseError;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
using ovt::syntax::h264::NalHeader;
using ovt::syntax::h264::Sps;

// ---- fixtures (NAL units with their emulation prevention bytes, as found in the Annex B streams) -----------

// x264 --preset medium --crf 24 --keyint 60 --min-keyint 60 --no-scenecut --bframes 3, 1920x1080 yuv420p 25 fps
// (chapter 14 recipe 26 parameters; High profile, level 4.0). The SPS carries two emulation prevention bytes.
constexpr std::string_view kGop60Sps = "67640028acd940780227e5c044000003000400000300f03c60c658";
// x264 yuv444p (High 4:4:4 Predictive), 352x288 29.97 fps, cqm=jvt, nal-hrd=vbr vbv-maxrate=5000 vbv-bufsize=10000,
// BT.709 colour description.
constexpr std::string_view k444Sps = "67f40015919b282c12d80b5010101400000fa40003a9818180004c4b40002625a9a8c01e2c5b2c";
// x264 interlaced (tff, MBAFF) 720x480 yuv420p, no B frames, SAR 8:9.
constexpr std::string_view kIntSps = "6764001eace402d1edff800400048800001f4800075300f8a15240";
// x264 yuv420p10 (High 10), 176x144 15 fps, --level 1b: level_idc 9, pic_order_cnt_type 2.
constexpr std::string_view kCqmSps = "676e0009a6cb20589d8088000003000800000300f078913240";

std::vector<uint8_t> from_hex(std::string_view hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

/// Writes spec descriptors into a byte vector (MSB first) for the hand-made fixtures.
class BitWriter {
 public:
  void u(unsigned n, uint64_t v) {
    for (unsigned i = n; i-- > 0;) {
      bits_.push_back(((v >> i) & 1U) != 0);
    }
  }
  void ue(uint32_t v) {
    const uint64_t code = uint64_t{v} + 1;
    unsigned len = 0;
    while ((code >> len) > 1) {
      ++len;
    }
    u(len, 0);
    u(len + 1, code);
  }
  void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * int64_t{v})); }
  void trailing() {
    u(1, 1);
    while (bits_.size() % 8 != 0) {
      u(1, 0);
    }
  }
  [[nodiscard]] std::vector<uint8_t> bytes() const {
    std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits_.size(); ++i) {
      if (bits_[i]) {
        out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80U >> (i % 8)));
      }
    }
    return out;
  }

 private:
  std::vector<bool> bits_;
};

struct Parsed {
  std::vector<uint8_t> rbsp;
  NalHeader header;
  std::optional<Sps> sps;
  SyntaxNodeBuffer tree;
  std::optional<ParseError> error;
};

Parsed parse_nal(const std::vector<uint8_t>& nal, bool recording = true) {
  Parsed p;
  p.rbsp = ovt::rbsp_from_nal(nal).rbsp;
  BitReader br(p.rbsp);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  try {
    p.header = ovt::syntax::h264::parse_nal_header(br, b);
    if (p.header.nal_unit_type == 7) {
      p.sps = ovt::syntax::h264::parse_sps(br, b);
    }
  } catch (const ParseError& e) {
    p.error = e;
  }
  p.tree = b.finish();
  p.tree.set_root_name("nal_unit");
  return p;
}

const SyntaxNodeData* find_node(const SyntaxNodeData* n, std::string_view name, int array_index = -1) {
  if (n == nullptr) {
    return nullptr;
  }
  // Array-element containers (begin_array_element) carry the same name as the leaf inside; return the leaf.
  if (n->name == name && !n->array_element && (array_index < 0 || n->array_index == array_index)) {
    return n;
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    if (const SyntaxNodeData* hit = find_node(c, name, array_index); hit != nullptr) {
      return hit;
    }
  }
  return nullptr;
}

std::vector<std::string> diag_codes(const SyntaxNodeData* n) {
  std::vector<std::string> out;
  if (n == nullptr) {
    return out;
  }
  for (const auto* d = n->first_diag; d != nullptr; d = d->next) {
    out.emplace_back(d->code);
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    const auto sub = diag_codes(c);
    out.insert(out.end(), sub.begin(), sub.end());
  }
  return out;
}

}  // namespace

// Acceptance test 1 (h264_sps_unit) on the recipe-26 parameters. trace_headers (FFmpeg 6.1): profile_idc 100,
// level_idc 40, seq_parameter_set_id at bit 32, pic_width_in_mbs_minus1 119 at bit 51, pic_height_in_map_units_minus1
// 67 at bit 64, frame_cropping_flag 1, frame_crop_bottom_offset 4, num_units_in_tick 1, time_scale 60,
// log2_max_mv_length_horizontal 11, max_num_reorder_frames 2, max_dec_frame_buffering 4, rbsp_stop_one_bit at 196.
TEST_CASE("h264_sps_unit", "[syntax][h264][fast]") {
  const auto p = parse_nal(from_hex(kGop60Sps));
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sps.has_value());
  const Sps& s = *p.sps;
  CHECK(p.rbsp.size() == 25);  // two emulation prevention bytes removed
  CHECK(p.header.nal_unit_type == 7);
  CHECK(p.header.nal_ref_idc == 3);
  CHECK(p.header.type_name == "seq_parameter_set_rbsp");
  CHECK(s.profile_idc == 100);
  CHECK(s.profile_name == "High");
  CHECK(s.level_idc == 40);
  CHECK(s.level_name == "4.0");
  CHECK(s.seq_parameter_set_id == 0);
  CHECK(s.chroma_format_idc == 1);
  CHECK(s.log2_max_pic_order_cnt_lsb_minus4 == 2);
  CHECK(s.max_num_ref_frames == 4);
  CHECK(s.pic_width_in_mbs_minus1 == 119);
  CHECK(s.pic_height_in_map_units_minus1 == 67);
  CHECK(s.frame_mbs_only_flag);
  CHECK(s.direct_8x8_inference_flag);
  CHECK(s.frame_cropping_flag);
  CHECK(s.frame_crop_bottom_offset == 4);
  CHECK(s.crop_x == 0);
  CHECK(s.crop_y == 0);
  CHECK(s.crop_width == 1920);
  CHECK(s.crop_height == 1080);
  CHECK(s.PicWidthInSamplesL == 1920);
  CHECK(s.FrameHeightInMbs == 68);
  CHECK(s.CropUnitX == 2);
  CHECK(s.CropUnitY == 2);
  CHECK(s.MaxFrameNum == 16);
  CHECK(s.MaxPicOrderCntLsb == 64);
  CHECK(s.MaxDpbMbs == 32768);
  CHECK(s.MaxDpbFrames == 4);  // 32768 / (120 * 68) = 4.01
  CHECK(s.vui_parameters_present_flag);
  CHECK(s.vui.aspect_ratio_idc == 1);
  CHECK(s.vui.num_units_in_tick == 1);
  CHECK(s.vui.time_scale == 60);
  CHECK(s.vui.frame_rate == 30.0);
  CHECK(s.vui.bitstream_restriction_flag);
  CHECK(s.vui.log2_max_mv_length_horizontal == 11);
  CHECK(s.vui.max_num_reorder_frames == 2);
  CHECK(s.vui.max_dec_frame_buffering == 4);
  CHECK_FALSE(s.vui.CpbDpbDelaysPresentFlag);
  CHECK(s.color_description.chroma_format == 1);
  CHECK(s.color_description.bit_depth == 8);
  CHECK(s.color_description.primaries == 2);  // no colour description: Unspecified
  CHECK(s.span.bit_offset == 8);
  CHECK(s.span.bit_length == 192);  // 25 bytes - 1 header byte

  const SyntaxNodeData* root = p.tree.root();
  REQUIRE(root != nullptr);
  CHECK(root->name == "nal_unit");
  const auto* id = find_node(root, "seq_parameter_set_id");
  REQUIRE(id != nullptr);
  CHECK(id->span.bit_offset == 32);
  CHECK(id->span.bit_length == 1);
  CHECK(id->descriptor == "ue(v)");
  const auto* width = find_node(root, "pic_width_in_mbs_minus1");
  REQUIRE(width != nullptr);
  CHECK(width->span.bit_offset == 51);
  CHECK(width->span.bit_length == 13);
  CHECK(width->uvalue == 119);
  const auto* level = find_node(root, "level_idc");
  REQUIRE(level != nullptr);
  CHECK(level->enum_name == "4.0");
  const auto* stop = find_node(root, "rbsp_stop_one_bit");
  REQUIRE(stop != nullptr);
  CHECK(stop->span.bit_offset == 196);
  const auto* crop = find_node(root, "crop_rect");
  REQUIRE(crop != nullptr);
  CHECK(crop->derived == "crop_rect = 1920x1080 at (0,0)");
  CHECK(diag_codes(root).empty());

  // §7.7.1 trace lines equal trace_headers' offsets and values.
  const std::string trace = ovt::syntax::to_trace(p.tree, p.rbsp);
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring("8 profile_idc 01100100 = 100\n"));
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring("51 pic_width_in_mbs_minus1 0000001111000 = 119\n"));
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring("83 frame_crop_bottom_offset 00101 = 4\n"));
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring("191 max_dec_frame_buffering 00101 = 4\n"));
}

// trace_headers: profile_idc 244, level_idc 21, chroma_format_idc 3 (bit 33), separate_colour_plane_flag 0,
// video_format 5, colour_primaries/transfer/matrix 1, num_units_in_tick 1001, time_scale 60000, cpb_cnt_minus1 0,
// bit_rate_scale 0, cpb_size_scale 3, bit_rate_value_minus1[0] 78124 (bit 193), cpb_size_value_minus1[0] 78124,
// cbr_flag[0] 0, initial_cpb_removal_delay_length_minus1 19, cpb_removal_delay_length_minus1 10,
// dpb_output_delay_length_minus1 6, time_offset_length 0, low_delay_hrd_flag 0 (bit 281).
TEST_CASE("h264_sps_444_hrd", "[syntax][h264][fast]") {
  const auto p = parse_nal(from_hex(k444Sps));
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sps.has_value());
  const Sps& s = *p.sps;
  CHECK(s.profile_idc == 244);
  CHECK(s.profile_name == "High 4:4:4 Predictive");
  CHECK(s.level_name == "2.1");
  CHECK(s.chroma_format_idc == 3);
  CHECK_FALSE(s.separate_colour_plane_flag);
  CHECK(s.ChromaArrayType == 3);
  CHECK(s.SubWidthC == 1);
  CHECK(s.SubHeightC == 1);
  CHECK(s.CropUnitX == 1);
  CHECK(s.CropUnitY == 1);
  CHECK(s.crop_width == 352);
  CHECK(s.crop_height == 288);
  CHECK(s.MaxDpbMbs == 4752);
  CHECK(s.MaxDpbFrames == 12);  // 4752 / (22 * 18) = 12
  CHECK(s.vui.video_signal_type_present_flag);
  CHECK(s.vui.video_format == 5);
  CHECK(s.vui.colour_description_present_flag);
  CHECK(s.vui.colour_primaries == 1);
  CHECK(s.vui.transfer_characteristics == 1);
  CHECK(s.vui.matrix_coefficients == 1);
  CHECK(s.color_description.primaries == 1);
  CHECK(s.color_description.matrix == 1);
  CHECK(s.color_description.full_range == 0);
  CHECK(s.color_description.chroma_format == 3);
  CHECK(s.vui.num_units_in_tick == 1001);
  CHECK(s.vui.time_scale == 60000);
  CHECK(s.vui.nal_hrd_parameters_present_flag);
  CHECK(s.vui.CpbDpbDelaysPresentFlag);
  CHECK_FALSE(s.vui.low_delay_hrd_flag);
  const auto& hrd = s.vui.nal_hrd;
  CHECK(hrd.cpb_cnt_minus1 == 0);
  CHECK(hrd.bit_rate_scale == 0);
  CHECK(hrd.cpb_size_scale == 3);
  REQUIRE(hrd.bit_rate_value_minus1.size() == 1);
  CHECK(hrd.bit_rate_value_minus1[0] == 78124);
  CHECK(hrd.cpb_size_value_minus1[0] == 78124);
  CHECK_FALSE(hrd.cbr_flag[0]);
  CHECK(hrd.BitRate[0] == 5000000);   // 78125 * 2^6
  CHECK(hrd.CpbSize[0] == 10000000);  // 78125 * 2^(4+3)
  CHECK(hrd.initial_cpb_removal_delay_length_minus1 == 19);
  CHECK(hrd.cpb_removal_delay_length_minus1 == 10);
  CHECK(hrd.dpb_output_delay_length_minus1 == 6);
  CHECK(hrd.time_offset_length == 0);
  const auto* br_node = find_node(p.tree.root(), "bit_rate_value_minus1");
  REQUIRE(br_node != nullptr);
  CHECK(br_node->span.bit_offset == 193);
  CHECK(br_node->array_index == 0);
  const auto* ld = find_node(p.tree.root(), "low_delay_hrd_flag");
  REQUIRE(ld != nullptr);
  CHECK(ld->span.bit_offset == 281);
  const auto* prim = find_node(p.tree.root(), "colour_primaries");
  REQUIRE(prim != nullptr);
  CHECK(prim->enum_name == "BT.709");
  const auto* cf = find_node(p.tree.root(), "chroma_format_idc");
  REQUIRE(cf != nullptr);
  CHECK(cf->enum_name == "4:4:4");
  CHECK(cf->span.bit_offset == 33);
}

// trace_headers: level_idc 30, frame_mbs_only_flag 0 (bit 67), mb_adaptive_frame_field_flag 1, pic_width_in_mbs_minus1
// 44, pic_height_in_map_units_minus1 14, aspect_ratio_idc 255, sar_width 8, sar_height 9, pic_struct_present_flag 1.
TEST_CASE("h264_sps_interlaced_mbaff", "[syntax][h264][fast]") {
  const auto p = parse_nal(from_hex(kIntSps));
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sps.has_value());
  const Sps& s = *p.sps;
  CHECK(s.level_name == "3.0");
  CHECK_FALSE(s.frame_mbs_only_flag);
  CHECK(s.mb_adaptive_frame_field_flag);
  CHECK(s.PicWidthInMbs == 45);
  CHECK(s.PicHeightInMapUnits == 15);
  CHECK(s.FrameHeightInMbs == 30);
  CHECK(s.PicSizeInMapUnits == 675);
  CHECK(s.CropUnitY == 4);  // SubHeightC * (2 - frame_mbs_only_flag)
  CHECK(s.crop_width == 720);
  CHECK(s.crop_height == 480);
  CHECK(s.MaxDpbMbs == 8100);
  CHECK(s.MaxDpbFrames == 6);  // 8100 / (45 * 30) = 6
  CHECK(s.vui.aspect_ratio_idc == 255);
  CHECK(s.vui.sar_width == 8);
  CHECK(s.vui.sar_height == 9);
  CHECK(s.vui.pic_struct_present_flag);
  CHECK(s.vui.max_num_reorder_frames == 0);
  const auto* ar = find_node(p.tree.root(), "aspect_ratio_idc");
  REQUIRE(ar != nullptr);
  CHECK(ar->enum_name == "Extended_SAR");
  const auto* fmo = find_node(p.tree.root(), "frame_mbs_only_flag");
  REQUIRE(fmo != nullptr);
  CHECK(fmo->span.bit_offset == 67);
}

// trace_headers: profile_idc 110, level_idc 9 (level 1b), bit_depth_luma_minus8 2, bit_depth_chroma_minus8 2,
// pic_order_cnt_type 2 (bit 45), pic_width_in_mbs_minus1 10, pic_height_in_map_units_minus1 8.
TEST_CASE("h264_sps_high10_level1b", "[syntax][h264][fast]") {
  const auto sp = parse_nal(from_hex(kCqmSps));
  REQUIRE_FALSE(sp.error.has_value());
  REQUIRE(sp.sps.has_value());
  const Sps& s = *sp.sps;
  CHECK(s.profile_idc == 110);
  CHECK(s.profile_name == "High 10");
  CHECK(s.level_idc == 9);
  CHECK(s.level_name == "1b");
  CHECK(s.MaxDpbMbs == 396);
  CHECK(s.MaxDpbFrames == 4);  // 396 / (11 * 9) = 4
  CHECK(s.bit_depth_luma_minus8 == 2);
  CHECK(s.BitDepthY == 10);
  CHECK(s.BitDepthC == 10);
  CHECK(s.QpBdOffsetY == 12);
  CHECK(s.QpBdOffsetC == 12);
  CHECK(s.pic_order_cnt_type == 2);
  CHECK(s.color_description.bit_depth == 10);
  const auto* poc = find_node(sp.tree.root(), "pic_order_cnt_type");
  REQUIRE(poc != nullptr);
  CHECK(poc->span.bit_offset == 45);
  CHECK(find_node(sp.tree.root(), "MaxPicOrderCntLsb") == nullptr);  // only for pic_order_cnt_type 0
}

// Hand-made High SPS with seq_scaling_matrix_present_flag = 1: list 0 explicit (the delta_scale run of the x264
// cqm4iy fixture: 6 7 10 14 11 8 9 12 15 18 19 16 13 17 20 21 in zig-zag order), list 1 with nextScale reaching 0
// after two deltas (the rest repeats 16), list 6 (8x8) with delta -8 at j = 0 → UseDefaultScalingMatrixFlag.
TEST_CASE("h264_sps_scaling_lists", "[syntax][h264][fast]") {
  BitWriter w;
  w.u(8, 0x67);
  w.u(8, 100);  // High
  w.u(8, 0);
  w.u(8, 31);
  w.ue(0);    // seq_parameter_set_id
  w.ue(1);    // chroma_format_idc
  w.ue(0);    // bit_depth_luma_minus8
  w.ue(0);    // bit_depth_chroma_minus8
  w.u(1, 0);  // qpprime_y_zero_transform_bypass_flag
  w.u(1, 1);  // seq_scaling_matrix_present_flag
  const std::array<bool, 8> present{true, true, false, false, false, false, true, false};
  const std::array<int32_t, 16> list0{-2, 1, 3, 4, -3, -3, 1, 3, 3, 3, 1, -3, -3, 4, 3, 1};
  for (size_t i = 0; i < present.size(); ++i) {
    w.u(1, present.at(i) ? 1 : 0);
    if (i == 0) {
      for (const int32_t d : list0) {
        w.se(d);
      }
    } else if (i == 1) {
      w.se(8);
      w.se(-16);
    } else if (i == 6) {
      w.se(-8);
    }
  }
  w.ue(0);    // log2_max_frame_num_minus4
  w.ue(2);    // pic_order_cnt_type
  w.ue(1);    // max_num_ref_frames
  w.u(1, 0);  // gaps_in_frame_num_value_allowed_flag
  w.ue(10);   // pic_width_in_mbs_minus1
  w.ue(8);    // pic_height_in_map_units_minus1
  w.u(1, 1);  // frame_mbs_only_flag
  w.u(1, 1);  // direct_8x8_inference_flag
  w.u(1, 0);  // frame_cropping_flag
  w.u(1, 0);  // vui_parameters_present_flag
  w.trailing();
  const auto p = parse_nal(w.bytes());
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sps.has_value());
  const Sps& s = *p.sps;
  CHECK(s.seq_scaling_matrix_present_flag);
  for (size_t i = 0; i < present.size(); ++i) {
    CHECK(s.scaling_list.at(i).present == present.at(i));
  }
  CHECK(s.scaling_list[0].size == 16);
  CHECK(s.scaling_list[6].size == 64);
  const std::array<uint8_t, 16> values0{6, 7, 10, 14, 11, 8, 9, 12, 15, 18, 19, 16, 13, 17, 20, 21};
  for (size_t j = 0; j < values0.size(); ++j) {
    CHECK(s.scaling_list[0].list.at(j) == values0.at(j));
    CHECK(s.scaling_list[1].list.at(j) == 16);  // 8 then -16: nextScale 0 → the rest repeats lastScale 16
  }
  CHECK(s.scaling_list[0].delta_scale[0] == -2);
  CHECK(s.scaling_list[0].delta_scale[15] == 1);
  CHECK_FALSE(s.scaling_list[0].UseDefaultScalingMatrixFlag);
  CHECK_FALSE(s.scaling_list[1].UseDefaultScalingMatrixFlag);
  CHECK(s.scaling_list[6].UseDefaultScalingMatrixFlag);
  CHECK(s.scaling_list[6].delta_scale[0] == -8);
  CHECK(s.crop_width == 176);
  const auto* ds = find_node(p.tree.root(), "delta_scale", 15);  // list 0, entry 15
  REQUIRE(ds != nullptr);
  CHECK(ds->ivalue == 1);
  const auto* flag6 = find_node(p.tree.root(), "seq_scaling_list_present_flag", 6);
  REQUIRE(flag6 != nullptr);
  CHECK(flag6->bvalue);
  const auto* use_default = find_node(p.tree.root(), "UseDefaultScalingMatrixFlag");
  REQUIRE(use_default != nullptr);
  // The trace prints both indices (list, entry) for a scaling-list coefficient.
  const std::string trace = ovt::syntax::to_trace(p.tree, p.rbsp);
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(" delta_scale[0][0] 00101 = -2\n"));
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(" delta_scale[1][1] 00000100001 = -16\n"));
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(" delta_scale[6][0] 000010001 = -8\n"));
  CHECK(diag_codes(p.tree.root()).empty());
}

TEST_CASE("h264_sps_pic_order_cnt_type1_and_defaults", "[syntax][h264][fast]") {
  // Hand-made Main profile SPS: pic_order_cnt_type 1 with two offsets, no VUI, no cropping.
  BitWriter w;
  w.u(8, 0x67);  // nal header: ref_idc 3, type 7
  w.u(8, 77);    // profile_idc Main
  w.u(8, 0);     // constraint flags + reserved
  w.u(8, 31);    // level 3.1
  w.ue(3);       // seq_parameter_set_id
  w.ue(2);       // log2_max_frame_num_minus4
  w.ue(1);       // pic_order_cnt_type
  w.u(1, 0);     // delta_pic_order_always_zero_flag
  w.se(-3);      // offset_for_non_ref_pic
  w.se(2);       // offset_for_top_to_bottom_field
  w.ue(2);       // num_ref_frames_in_pic_order_cnt_cycle
  w.se(4);       // offset_for_ref_frame[0]
  w.se(-1);      // offset_for_ref_frame[1]
  w.ue(2);       // max_num_ref_frames
  w.u(1, 1);     // gaps_in_frame_num_value_allowed_flag
  w.ue(21);      // pic_width_in_mbs_minus1 (352)
  w.ue(17);      // pic_height_in_map_units_minus1 (288)
  w.u(1, 1);     // frame_mbs_only_flag
  w.u(1, 0);     // direct_8x8_inference_flag
  w.u(1, 0);     // frame_cropping_flag
  w.u(1, 0);     // vui_parameters_present_flag
  w.trailing();
  const auto p = parse_nal(w.bytes());
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sps.has_value());
  const Sps& s = *p.sps;
  CHECK(s.profile_name == "Main");
  CHECK_FALSE(s.high_profile_syntax);
  CHECK(s.chroma_format_idc == 1);  // inferred
  CHECK(s.seq_parameter_set_id == 3);
  CHECK(s.MaxFrameNum == 64);
  CHECK(s.pic_order_cnt_type == 1);
  CHECK(s.offset_for_non_ref_pic == -3);
  CHECK(s.offset_for_top_to_bottom_field == 2);
  REQUIRE(s.offset_for_ref_frame.size() == 2);
  CHECK(s.offset_for_ref_frame[0] == 4);
  CHECK(s.offset_for_ref_frame[1] == -1);
  CHECK(s.ExpectedDeltaPerPicOrderCntCycle == 3);
  CHECK(s.gaps_in_frame_num_value_allowed_flag);
  CHECK(s.crop_width == 352);
  CHECK(s.crop_height == 288);
  CHECK(s.MaxDpbMbs == 18000);
  CHECK(s.MaxDpbFrames == 16);  // 18000 / 396 = 45, capped at 16
  CHECK_FALSE(s.vui_parameters_present_flag);
  CHECK(s.vui.frame_rate == 0.0);
  const auto* off1 = find_node(p.tree.root(), "offset_for_ref_frame", 1);
  REQUIRE(off1 != nullptr);
  CHECK(off1->ivalue == -1);
  const auto* exp = find_node(p.tree.root(), "ExpectedDeltaPerPicOrderCntCycle");
  REQUIRE(exp != nullptr);
  CHECK(exp->derived == "ExpectedDeltaPerPicOrderCntCycle = 3");
  CHECK(diag_codes(p.tree.root()).empty());
}

TEST_CASE("h264_sps_level_1b_constraint_set3_and_unknown_profile", "[syntax][h264][fast]") {
  auto make = [](uint32_t profile, bool cs3, uint32_t level) {
    BitWriter w;
    w.u(8, 0x67);
    w.u(8, profile);
    w.u(8, cs3 ? 0x10U : 0U);
    w.u(8, level);
    w.ue(0);  // seq_parameter_set_id
    if (profile == 100) {
      w.ue(1);    // chroma_format_idc
      w.ue(0);    // bit_depth_luma_minus8
      w.ue(0);    // bit_depth_chroma_minus8
      w.u(1, 0);  // qpprime_y_zero_transform_bypass_flag
      w.u(1, 0);  // seq_scaling_matrix_present_flag
    }
    w.ue(0);    // log2_max_frame_num_minus4
    w.ue(2);    // pic_order_cnt_type
    w.ue(1);    // max_num_ref_frames
    w.u(1, 0);  // gaps
    w.ue(10);   // width 176
    w.ue(8);    // height 144
    w.u(1, 1);  // frame_mbs_only_flag
    w.u(1, 1);  // direct_8x8_inference_flag
    w.u(1, 0);  // cropping
    w.u(1, 0);  // vui
    w.trailing();
    return w.bytes();
  };
  SECTION("Baseline level_idc 11 + constraint_set3_flag is level 1b") {
    const auto p = parse_nal(make(66, true, 11));
    REQUIRE(p.sps.has_value());
    CHECK(p.sps->level_name == "1b");
    CHECK(p.sps->MaxDpbMbs == 396);
    CHECK(p.sps->profile_name == "Baseline");
    CHECK(diag_codes(p.tree.root()).empty());
  }
  SECTION("High level_idc 11 + constraint_set3_flag stays level 1.1") {
    const auto p = parse_nal(make(100, true, 11));
    REQUIRE(p.sps.has_value());
    CHECK(p.sps->level_name == "1.1");
    CHECK(p.sps->MaxDpbMbs == 900);
  }
  SECTION("unknown profile_idc and level_idc warn SYN-H264-0003 and parse on") {
    const auto p = parse_nal(make(99, false, 33));
    REQUIRE_FALSE(p.error.has_value());
    REQUIRE(p.sps.has_value());
    CHECK(p.sps->profile_name == "unknown");
    CHECK(p.sps->level_name.empty());
    CHECK(p.sps->MaxDpbMbs == 0);
    CHECK(p.sps->crop_width == 176);
    const auto codes = diag_codes(p.tree.root());
    CHECK(std::ranges::count(codes, "SYN-H264-0003") == 2);
    CHECK(find_node(p.tree.root(), "MaxDpbFrames") == nullptr);
  }
}

TEST_CASE("h264_sps_range_errors", "[syntax][h264][fast]") {
  auto sps_with_id = [](uint32_t id) {
    BitWriter w;
    w.u(8, 0x67);
    w.u(8, 66);
    w.u(8, 0);
    w.u(8, 30);
    w.ue(id);
    w.trailing();
    return w.bytes();
  };
  SECTION("seq_parameter_set_id 32") {
    const auto p = parse_nal(sps_with_id(32));
    REQUIRE(p.error.has_value());
    CHECK(p.error->code == "SYN-H264-0002");
    CHECK_THAT(p.error->what(), Catch::Matchers::ContainsSubstring("seq_parameter_set_id out of range"));
  }
  SECTION("cpb_cnt_minus1 32 in hrd_parameters") {
    BitWriter w;
    w.ue(32);
    w.u(8, 0);
    w.trailing();
    const auto bytes = w.bytes();
    BitReader br(bytes);
    SyntaxBuilder b(br);
    CHECK_THROWS_AS(ovt::syntax::h264::parse_hrd_parameters(br, b), ParseError);
    const auto tree = b.finish();
    CHECK(find_node(tree.root(), "cpb_cnt_minus1") != nullptr);
  }
  SECTION("delta_scale below -128 (fuzz regression ubsan_scaling_delta_max)") {
    // High profile SPS whose first scaling list starts with delta_scale at the se(v) extreme; before the #388 bound
    // the nextScale arithmetic overflowed int32.
    const auto p = parse_nal(from_hex("6764001fad8000000300fffffffefffe02d0589c80"));
    REQUIRE(p.error.has_value());
    CHECK(p.error->code == "SYN-H264-0002");
    CHECK_THAT(p.error->what(), Catch::Matchers::ContainsSubstring("delta_scale out of range"));
  }
  SECTION("pic_height_in_map_units_minus1 2^31-1 (fuzz regression ubsan_sps_frame_mbs_zero)") {
    // frame_mbs_only_flag = 0 made FrameHeightInMbs wrap to 0 and MaxDpbFrames divide by zero.
    const auto p = parse_nal(from_hex("6742001eda4000000300400000030012"));
    REQUIRE(p.error.has_value());
    CHECK(p.error->code == "SYN-H264-0002");
    CHECK_THAT(p.error->what(), Catch::Matchers::ContainsSubstring("pic_height_in_map_units_minus1 out of range"));
  }
  SECTION("pic_width_in_mbs_minus1 65536 is rejected, 65535 is accepted") {
    auto sps_with_width = [](uint32_t width_minus1) {
      BitWriter w;
      w.u(8, 0x67);
      w.u(8, 66);
      w.u(8, 0);
      w.u(8, 30);
      w.ue(0);    // seq_parameter_set_id
      w.ue(0);    // log2_max_frame_num_minus4
      w.ue(2);    // pic_order_cnt_type
      w.ue(1);    // max_num_ref_frames
      w.u(1, 0);  // gaps_in_frame_num_value_allowed_flag
      w.ue(width_minus1);
      w.ue(65535);  // pic_height_in_map_units_minus1
      w.u(1, 0);    // frame_mbs_only_flag
      w.u(1, 0);    // mb_adaptive_frame_field_flag
      w.u(1, 1);    // direct_8x8_inference_flag
      w.u(1, 0);    // frame_cropping_flag
      w.u(1, 0);    // vui_parameters_present_flag
      w.trailing();
      return w.bytes();
    };
    const auto bad = parse_nal(sps_with_width(65536));
    REQUIRE(bad.error.has_value());
    CHECK(bad.error->code == "SYN-H264-0002");
    const auto ok = parse_nal(sps_with_width(65535));
    REQUIRE_FALSE(ok.error.has_value());
    REQUIRE(ok.sps.has_value());
    CHECK(ok.sps->PicWidthInMbs == 65536);
    CHECK(ok.sps->FrameHeightInMbs == 131072);
    CHECK(ok.sps->PicSizeInMapUnits == uint64_t{65536} * 65536);
    CHECK(ok.sps->MaxDpbFrames == 0);
  }
  SECTION("truncated SPS propagates the reader error") {
    const auto p = parse_nal(from_hex("6764"));
    REQUIRE(p.error.has_value());
    CHECK(p.error->code == "BR-0001");
  }
}

TEST_CASE("h264_nal_header_extensions", "[syntax][h264][fast]") {
  SECTION("prefix NAL (14) with svc_extension_flag = 1: SVC header") {
    // 0x6e = 0 11 01110; then svc_extension_flag 1, idr_flag 1, priority_id 5 (000101), no_inter_layer_pred_flag 0,
    // dependency_id 2 (010), quality_id 1 (0001), temporal_id 3 (011), use_ref_base_pic_flag 0, discardable_flag 1,
    // output_flag 1, reserved_three_2bits 11.
    BitWriter w;
    w.u(8, 0x6e);
    w.u(1, 1);
    w.u(1, 1);
    w.u(6, 5);
    w.u(1, 0);
    w.u(3, 2);
    w.u(4, 1);
    w.u(3, 3);
    w.u(1, 0);
    w.u(1, 1);
    w.u(1, 1);
    w.u(2, 3);
    const auto p = parse_nal(w.bytes());
    REQUIRE_FALSE(p.error.has_value());
    CHECK(p.header.nal_unit_type == 14);
    CHECK(p.header.type_name == "prefix_nal_unit_rbsp");
    CHECK(p.header.svc_extension_flag);
    CHECK(p.header.has_svc_extension);
    CHECK(p.header.svc.idr_flag);
    CHECK(p.header.svc.priority_id == 5);
    CHECK(p.header.svc.dependency_id == 2);
    CHECK(p.header.svc.quality_id == 1);
    CHECK(p.header.svc.temporal_id == 3);
    CHECK(p.header.svc.discardable_flag);
    CHECK(p.header.svc.output_flag);
    CHECK(p.header.temporal_id == 3);
    CHECK(p.header.layer_id == 2);
    CHECK_FALSE(p.header.IdrPicFlag);
    CHECK(p.header.span.bit_length == 32);
    CHECK(find_node(p.tree.root(), "nal_unit_header_svc_extension") != nullptr);
    CHECK(diag_codes(p.tree.root()).empty());
  }
  SECTION("slice extension (20) with svc_extension_flag = 0: MVC header") {
    // 0x74 = 0 11 10100; svc_extension_flag 0, non_idr_flag 1, priority_id 0, view_id 0x155 (10 bits),
    // temporal_id 2, anchor_pic_flag 0, inter_view_flag 1, reserved_one_bit 1.
    BitWriter w;
    w.u(8, 0x74);
    w.u(1, 0);
    w.u(1, 1);
    w.u(6, 0);
    w.u(10, 0x155);
    w.u(3, 2);
    w.u(1, 0);
    w.u(1, 1);
    w.u(1, 1);
    const auto p = parse_nal(w.bytes());
    REQUIRE_FALSE(p.error.has_value());
    CHECK(p.header.has_mvc_extension);
    CHECK(p.header.mvc.non_idr_flag);
    CHECK(p.header.mvc.view_id == 0x155);
    CHECK(p.header.mvc.temporal_id == 2);
    CHECK(p.header.mvc.inter_view_flag);
    CHECK(p.header.layer_id == 0x155);
    CHECK(p.header.temporal_id == 2);
    CHECK(diag_codes(p.tree.root()).empty());
  }
  SECTION("3D-AVC slice extension (21) with avc_3d_extension_flag = 1") {
    // 0x75 = 0 11 10101; avc_3d_extension_flag 1, view_idx 7, depth_flag 1, non_idr_flag 0, temporal_id 1,
    // anchor_pic_flag 1, inter_view_flag 0.
    BitWriter w;
    w.u(8, 0x75);
    w.u(1, 1);
    w.u(8, 7);
    w.u(1, 1);
    w.u(1, 0);
    w.u(3, 1);
    w.u(1, 1);
    w.u(1, 0);
    const auto p = parse_nal(w.bytes());
    REQUIRE_FALSE(p.error.has_value());
    CHECK(p.header.avc_3d_extension_flag);
    CHECK(p.header.has_3davc_extension);
    CHECK(p.header.avc_3d.view_idx == 7);
    CHECK(p.header.avc_3d.depth_flag);
    CHECK(p.header.avc_3d.anchor_pic_flag);
    CHECK(p.header.layer_id == 7);
    CHECK(p.header.temporal_id == 1);
    CHECK(p.header.span.bit_length == 24);
  }
  SECTION("reserved nal_unit_type and forbidden_zero_bit are SYN-H264-0001 Diagnostics, not errors") {
    const auto reserved = parse_nal(from_hex("7100"));  // 0 11 10001 → type 17
    REQUIRE_FALSE(reserved.error.has_value());
    CHECK(reserved.header.reserved_type);
    CHECK(reserved.header.type_name == "reserved");
    CHECK(diag_codes(reserved.tree.root()) == std::vector<std::string>{"SYN-H264-0001"});
    const auto forbidden = parse_nal(from_hex("e100"));  // 1 11 00001
    CHECK(forbidden.header.forbidden_zero_bit == 1);
    CHECK(forbidden.header.nal_unit_type == 1);
    CHECK(diag_codes(forbidden.tree.root()) == std::vector<std::string>{"SYN-H264-0001"});
    const auto idr = parse_nal(from_hex("6500"));
    CHECK(idr.header.IdrPicFlag);
    CHECK(idr.header.type_name == "slice_layer_without_partitioning_rbsp (IDR)");
  }
}

TEST_CASE("h264_ps_deterministic_and_non_recording", "[syntax][h264][fast]") {
  const auto fixture = GENERATE(kGop60Sps, k444Sps, kIntSps, kCqmSps);
  const auto a = parse_nal(from_hex(fixture));
  const auto b = parse_nal(from_hex(fixture));
  REQUIRE_FALSE(a.error.has_value());
  CHECK(a.tree.to_flatbuffer() == b.tree.to_flatbuffer());
  CHECK(ovt::syntax::to_json(a.tree) == ovt::syntax::to_json(b.tree));
  const auto quiet = parse_nal(from_hex(fixture), /*recording=*/false);
  REQUIRE_FALSE(quiet.error.has_value());
  CHECK(quiet.tree.empty());
  REQUIRE(quiet.sps.has_value());
  CHECK(quiet.sps->profile_idc == a.sps->profile_idc);
  CHECK(quiet.sps->crop_width == a.sps->crop_width);
  CHECK(quiet.sps->MaxDpbFrames == a.sps->MaxDpbFrames);
  CHECK(quiet.sps->span.bit_length == a.sps->span.bit_length);
}

// ---- corpus-backed acceptance tests (skipped when the corpus cache lacks the stream) ------------------------

namespace {

std::vector<uint8_t> first_sps(std::string_view corpus_id) {
  const auto path = ovt::test::corpus_or_skip(corpus_id);
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  std::vector<uint8_t> sps;
  ovt::syntax::scan_annexb(*file, [&](const ovt::syntax::ScannedUnit& u) {
    if (!u.bytes.empty() && (u.bytes[0] & 0x1FU) == 7 && sps.empty()) {
      sps.assign(u.bytes.begin(), u.bytes.end());
    }
  });
  REQUIRE_FALSE(sps.empty());
  return sps;
}

}  // namespace

TEST_CASE("h264_sps_unit_corpus_gop60", "[syntax][h264][fast][needs-corpus]") {
  const auto p = parse_nal(first_sps("syn_h264_gop60_1080p"));
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sps.has_value());
  CHECK(p.sps->profile_idc == 100);
  CHECK(p.sps->pic_width_in_mbs_minus1 == 119);
  CHECK(p.sps->pic_height_in_map_units_minus1 == 67);
  CHECK(p.sps->frame_cropping_flag);
  CHECK(p.sps->frame_crop_bottom_offset == 4);
  CHECK(p.sps->crop_width == 1920);
  CHECK(p.sps->crop_height == 1080);
  const auto* id = find_node(p.tree.root(), "seq_parameter_set_id");
  REQUIRE(id != nullptr);
  CHECK(id->span.bit_offset == 32);
}
