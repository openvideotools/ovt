// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 vui_parameters(), scaling_list_data() and st_ref_pic_set() tests (chapter 7 §7.3.5 rows 5, 7 and 9,
// §7.3.10.3; WI-1.112). Hand-assembled structures checked against the E.2.1 semantics, the Table 7-5/7-6 defaults
// and equations 7-59..7-62.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "h265_test_util.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/scaling_list.hpp"
#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"
#include "ovt/cxx/syntax/h265/vui.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::ParseError;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
using ovt::test::h265::BitWriter;
using ovt::test::h265::find_child;
namespace h265 = ovt::syntax::h265;

}  // namespace

// ---- row 7: vui_parameters ------------------------------------------------------------------------------------

TEST_CASE("h265_vui_parameters: extended SAR, BT.2020 PQ colour, 59.94 fps and bitstream restriction",
          "[syntax][h265][fast]") {
  BitWriter w;
  w.flag(true);
  w.u(8, 255);  // EXTENDED_SAR
  w.u(16, 1);
  w.u(16, 1);
  w.flag(true);  // overscan_info_present_flag
  w.flag(true);  // overscan_appropriate_flag
  w.flag(true);  // video_signal_type_present_flag
  w.u(3, 5);
  w.flag(false);  // limited range
  w.flag(true);
  w.u(8, 9);      // BT.2020 primaries
  w.u(8, 16);     // PQ
  w.u(8, 9);      // BT.2020-NCL
  w.flag(false);  // chroma_loc_info_present_flag
  w.flag(false);  // neutral_chroma_indication_flag
  w.flag(false);  // field_seq_flag
  w.flag(true);   // frame_field_info_present_flag
  w.flag(true);   // default_display_window_flag
  w.ue(1);
  w.ue(2);
  w.ue(3);
  w.ue(4);
  w.flag(true);  // vui_timing_info_present_flag
  w.u(32, 1001);
  w.u(32, 60000);
  w.flag(true);  // vui_poc_proportional_to_timing_flag
  w.ue(0);
  w.flag(false);  // vui_hrd_parameters_present_flag
  w.flag(true);   // bitstream_restriction_flag
  w.flag(true);   // tiles_fixed_structure_flag
  w.flag(false);  // motion_vectors_over_pic_boundaries_flag
  w.flag(true);   // restricted_ref_pic_lists_flag
  w.ue(0);        // min_spatial_segmentation_idc
  w.ue(2);        // max_bytes_per_pic_denom
  w.ue(1);        // max_bits_per_min_cu_denom
  w.ue(15);
  w.ue(15);
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  const h265::VuiParameters v = h265::parse_vui_parameters(br, b, 0, 1);
  CHECK(v.aspect_ratio_idc == 255);
  CHECK(v.sar_width == 1);
  CHECK(v.sar_height == 1);
  CHECK(v.overscan_appropriate_flag);
  CHECK(v.colour_primaries == 9);
  CHECK(v.transfer_characteristics == 16);
  CHECK(v.matrix_coeffs == 9);
  CHECK(v.def_disp_win_bottom_offset == 4);
  CHECK(v.vui_num_units_in_tick == 1001);
  CHECK(v.vui_time_scale == 60000);
  CHECK(v.frame_rate > 59.93);
  CHECK(v.frame_rate < 59.95);
  CHECK(v.tiles_fixed_structure_flag);
  CHECK_FALSE(v.motion_vectors_over_pic_boundaries_flag);
  CHECK(v.log2_max_mv_length_vertical == 15);
  CHECK(v.color.primaries == 9);
  CHECK(v.color.transfer == 16);
  CHECK(v.color.matrix == 9);
  CHECK(v.color.full_range == 0);
  CHECK(v.color.chroma_siting == 3);  // HEVC default for 4:2:0 without chroma_loc_info
  CHECK(br.bit_pos() == w.size_bits());
  const SyntaxNodeBuffer tree = b.finish();
  const SyntaxNodeData* node = tree.root()->child(0);
  REQUIRE(node != nullptr);
  CHECK(node->name == "vui_parameters");
  const SyntaxNodeData* tc = find_child(node, "transfer_characteristics");
  REQUIRE(tc != nullptr);
  CHECK(tc->enum_name == "PQ");
  CHECK(h265::colour_primaries_name(1) == "BT.709");
  CHECK(h265::matrix_coeffs_name(14) == "ICtCp");
  CHECK(h265::transfer_characteristics_name(18) == "HLG");
  CHECK(h265::transfer_characteristics_name(200) == "reserved");
}

TEST_CASE("h265_vui_parameters: chroma_sample_loc_type_top_field maps to the OVT siting codes",
          "[syntax][h265][fast]") {
  for (const auto& [loc, siting] : {std::pair{0U, 1U}, std::pair{1U, 2U}, std::pair{2U, 3U}, std::pair{4U, 0U}}) {
    BitWriter w;
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(true);  // chroma_loc_info_present_flag
    w.ue(loc);
    w.ue(loc);
    w.u(4, 0);      // neutral, field_seq, frame_field_info, default_display_window
    w.flag(false);  // timing
    w.flag(false);  // bitstream_restriction
    const std::vector<uint8_t> data = w.bytes();
    BitReader br(data);
    SyntaxBuilder b(br);
    const h265::VuiParameters v = h265::parse_vui_parameters(br, b, 0, 1);
    CHECK(v.chroma_sample_loc_type_top_field == loc);
    CHECK(v.color.chroma_siting == siting);
  }
}

// ---- row 5: scaling_list_data ---------------------------------------------------------------------------------

TEST_CASE("h265_scaling_list: default and flat lists derive the Table 7-6 factors in diagonal order",
          "[syntax][h265][fast]") {
  const h265::ScalingListData d = h265::default_scaling_list_data();
  CHECK(d.from_default);
  CHECK(d.scaling_factor(0, 0, 3, 3) == 16);
  CHECK(d.scaling_factor(1, 0, 0, 0) == 16);
  CHECK(d.scaling_factor(1, 0, 7, 7) == 115);  // last diagonal entry of the intra list
  CHECK(d.scaling_factor(1, 3, 7, 7) == 91);   // inter list
  CHECK(d.scaling_factor(1, 0, 0, 1) == 16);   // diagScan[1] = (0, 1)
  CHECK(d.scaling_factor(1, 0, 7, 6) == 88);   // diagScan[62] = (7, 6) ... ScalingList[62] = 88
  CHECK(d.scaling_factor(2, 0, 0, 0) == 16);   // DC
  CHECK(d.scaling_factor(2, 0, 15, 15) == 115);
  CHECK(d.scaling_factor(2, 0, 14, 15) == 115);  // 2x2 replication
  CHECK(d.scaling_factor(3, 0, 31, 31) == 115);
  CHECK(d.scaling_factor(3, 3, 0, 0) == 16);
  CHECK(d.scaling_factor(3, 1, 31, 31) == 115);  // chroma 32x32 from the 16x16 list (4:4:4 rule)
  const h265::ScalingListData f = h265::flat_scaling_list_data();
  CHECK(f.flat);
  CHECK(f.scaling_factor(1, 0, 7, 7) == 16);
  CHECK(f.scaling_factor(3, 3, 31, 31) == 16);
}

TEST_CASE("h265_scaling_list: explicit coefficients, DC, reference prediction and default inference",
          "[syntax][h265][fast]") {
  BitWriter w;
  // sizeId 0: matrixId 0 explicit (coefficients 8+1, 8+1+1, ... ), 1 predicted from 0 (delta 1), 2..5 default (delta 0)
  w.flag(true);
  for (unsigned i = 0; i < 16; ++i) {
    w.se(1);
  }
  w.flag(false);
  w.ue(1);
  for (unsigned m = 2; m < 6; ++m) {
    w.flag(false);
    w.ue(0);
  }
  // sizeId 1: all default
  for (unsigned m = 0; m < 6; ++m) {
    w.flag(false);
    w.ue(0);
  }
  // sizeId 2: matrixId 0 explicit with dc -7 (=> 1) and zero deltas (list = 1 everywhere), 1 copies 0, rest default
  w.flag(true);
  w.se(-7);
  for (unsigned i = 0; i < 64; ++i) {
    w.se(0);
  }
  w.flag(false);
  w.ue(1);
  for (unsigned m = 2; m < 6; ++m) {
    w.flag(false);
    w.ue(0);
  }
  // sizeId 3: matrixId 0 default, matrixId 3 predicted from 0 (delta 1 => refMatrixId 0)
  w.flag(false);
  w.ue(0);
  w.flag(false);
  w.ue(1);
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  const h265::ScalingListData s = h265::parse_scaling_list_data(br, b);
  CHECK(br.bit_pos() == w.size_bits());
  CHECK(s.ScalingList[0][0][0] == 9);
  CHECK(s.ScalingList[0][0][15] == 24);
  CHECK(s.ScalingList[0][1] == s.ScalingList[0][0]);
  CHECK(s.scaling_list_pred_matrix_id_delta[0][1] == 1);
  CHECK(s.ScalingList[0][2][5] == 16);
  CHECK(s.ScalingList[1][0][63] == 115);
  CHECK(s.ScalingList[1][5][63] == 91);
  CHECK(s.scaling_list_dc_coef_minus8[0][0] == -7);
  CHECK(s.ScalingList[2][0][0] == 1);
  CHECK(s.ScalingList[2][0][63] == 1);
  CHECK(s.scaling_factor(2, 0, 0, 0) == 1);
  CHECK(s.scaling_factor(2, 0, 9, 9) == 1);
  CHECK(s.scaling_list_dc_coef_minus8[0][1] == -7);  // copied with the list
  CHECK(s.ScalingList[2][1][10] == 1);
  CHECK(s.scaling_list_dc_coef_minus8[0][2] == 8);  // inferred for the default list
  CHECK(s.ScalingList[3][3] == s.ScalingList[3][0]);
  CHECK(s.scaling_factor(3, 3, 31, 31) == 115);
  const SyntaxNodeBuffer tree = b.finish();
  const SyntaxNodeData* node = tree.root()->child(0);
  REQUIRE(node != nullptr);
  CHECK(node->name == "scaling_list_data");
  CHECK(node->child(0)->name == "sizeId");
  CHECK(node->child(0)->array_index == 0);
  CHECK(node->child(0)->child(1)->name == "matrixId");
  CHECK(node->child(0)->child(1)->array_index == 1);
  CHECK(node->child(0)->child(1)->child(1)->name == "scaling_list_pred_matrix_id_delta");
  CHECK(node->child(0)->child(1)->child(1)->uvalue == 1);
  CHECK(node->child(0)->child(1)->child(1)->array_index == 1);
}

// ---- row 9: st_ref_pic_set ------------------------------------------------------------------------------------

TEST_CASE("h265_st_ref_pic_set: explicit set then an inter-predicted set (equations 7-61/7-62)",
          "[syntax][h265][fast]") {
  BitWriter w;
  // set 0: S0 = {-1 used, -3 not used}, S1 = {+1 used}
  w.ue(2);
  w.ue(1);
  w.ue(0);
  w.flag(true);
  w.ue(1);
  w.flag(false);
  w.ue(0);
  w.flag(true);
  // set 1: predicted from set 0 with deltaRps = -1; flags for j = 0..3
  w.flag(true);  // inter_ref_pic_set_prediction_flag
  w.flag(true);  // delta_rps_sign
  w.ue(0);       // abs_delta_rps_minus1
  w.flag(true);  // used[0]
  w.flag(false);
  w.flag(true);  // use_delta[1]
  w.flag(false);
  w.flag(false);  // used[2] = 0, use_delta[2] = 0 (drops +1 - 1 = 0 anyway)
  w.flag(true);   // used[3] (the deltaRps entry)
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  std::vector<h265::StRefPicSet> sets;
  sets.push_back(h265::parse_st_ref_pic_set(br, b, 0, 2, sets, 4));
  sets.push_back(h265::parse_st_ref_pic_set(br, b, 1, 2, sets, 4));
  CHECK(br.bit_pos() == w.size_bits());
  const h265::StRefPicSet& s0 = sets[0];
  CHECK(s0.NumNegativePics == 2);
  CHECK(s0.NumPositivePics == 1);
  CHECK(s0.NumDeltaPocs == 3);
  CHECK(s0.DeltaPocS0[0] == -1);
  CHECK(s0.DeltaPocS0[1] == -3);
  CHECK(s0.UsedByCurrPicS0[0]);
  CHECK_FALSE(s0.UsedByCurrPicS0[1]);
  CHECK(s0.DeltaPocS1[0] == 1);
  CHECK(h265::st_ref_pic_set_summary(s0) == "S0: -1(used) -3 | S1: +1(used)");
  const h265::StRefPicSet& s1 = sets[1];
  CHECK(s1.inter_ref_pic_set_prediction_flag);
  CHECK(s1.RefRpsIdx == 0);
  CHECK(s1.deltaRps == -1);
  CHECK(s1.NumNegativePics == 3);
  CHECK(s1.NumPositivePics == 0);
  CHECK(s1.DeltaPocS0[0] == -1);
  CHECK(s1.DeltaPocS0[1] == -2);
  CHECK(s1.DeltaPocS0[2] == -4);
  CHECK(s1.UsedByCurrPicS0[0]);
  CHECK(s1.UsedByCurrPicS0[1]);
  CHECK_FALSE(s1.UsedByCurrPicS0[2]);
  CHECK(h265::st_ref_pic_set_summary(s1) == "S0: -1(used) -2(used) -4 | S1:");
  const SyntaxNodeBuffer tree = b.finish();
  REQUIRE(tree.root()->child_count() == 2);
  CHECK(tree.root()->child(1)->name == "st_ref_pic_set");
  CHECK(tree.root()->child(1)->array_index == 1);
  const SyntaxNodeData* summary = find_child(tree.root()->child(1), "DeltaPoc");
  REQUIRE(summary != nullptr);
  CHECK(summary->derived == "S0: -1(used) -2(used) -4 | S1:");
}

TEST_CASE("h265_scaling_list: scaling_list_delta_coef outside [-128, 127] throws SYN-H265-0007",
          "[syntax][h265][fast]") {
  BitWriter w;
  w.flag(true);  // sizeId 0, matrixId 0 explicit
  w.se(128);
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  bool threw = false;
  try {
    (void)h265::parse_scaling_list_data(br, b);
  } catch (const ParseError& e) {
    threw = true;
    CHECK(e.code == "SYN-H265-0007");
    CHECK(std::string_view{e.what()}.find("scaling_list_delta_coef") != std::string_view::npos);
  }
  CHECK(threw);
}

TEST_CASE("h265_st_ref_pic_set: delta_poc_s0_minus1 beyond 2^15 - 1 throws SYN-H265-0007", "[syntax][h265][fast]") {
  BitWriter w;
  w.ue(1);
  w.ue(0);
  w.ue(1U << 15);
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  std::vector<h265::StRefPicSet> none;
  bool threw = false;
  try {
    (void)h265::parse_st_ref_pic_set(br, b, 0, 1, none, 15);
  } catch (const ParseError& e) {
    threw = true;
    CHECK(e.code == "SYN-H265-0007");
    CHECK(std::string_view{e.what()}.find("delta_poc_s0_minus1") != std::string_view::npos);
  }
  CHECK(threw);
}

TEST_CASE("h265_st_ref_pic_set: num_negative_pics beyond 16 throws SYN-H265-0007", "[syntax][h265][fast]") {
  BitWriter w;
  w.ue(17);
  w.ue(0);
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  std::vector<h265::StRefPicSet> none;
  bool threw = false;
  try {
    (void)h265::parse_st_ref_pic_set(br, b, 0, 1, none, 15);
  } catch (const ParseError& e) {
    threw = true;
    CHECK(e.code == "SYN-H265-0007");
  }
  CHECK(threw);
}
