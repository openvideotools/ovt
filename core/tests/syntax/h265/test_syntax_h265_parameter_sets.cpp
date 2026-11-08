// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 VPS and SPS parser tests (chapter 7 §7.3.5 rows 2 and 4, WI-1.112). Part d of the work item adds the PPS
// tests and the corpus acceptance tests, whose expected values are hard-coded from the recorded oracle outputs
// committed as goldens under tests/golden/h265/.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "h265_test_util.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/profile_tier_level.hpp"
#include "ovt/cxx/syntax/h265/scaling_list.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
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
using ovt::test::h265::parse_nal;
using ovt::test::h265::write_general_profile;
namespace h265 = ovt::syntax::h265;

h265::Vps parse_vps_nal(const std::vector<uint8_t>& nal, SyntaxNodeBuffer* tree = nullptr) {
  return parse_nal(
      nal, [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) { return h265::parse_vps(br, b); }, tree);
}

}  // namespace

// ---- row 2: video_parameter_set_rbsp (hand-built) -------------------------------------------------------------

TEST_CASE("h265_vps: two sub-layers, timing info, one hrd_parameters() entry and the derived frame rate",
          "[syntax][h265][fast]") {
  BitWriter w;
  w.u(4, 3);        // vps_video_parameter_set_id
  w.flag(true);     // vps_base_layer_internal_flag
  w.flag(true);     // vps_base_layer_available_flag
  w.u(6, 0);        // vps_max_layers_minus1
  w.u(3, 1);        // vps_max_sub_layers_minus1
  w.flag(false);    // vps_temporal_id_nesting_flag
  w.u(16, 0xFFFF);  // vps_reserved_0xffff_16bits
  write_general_profile(w, 1, {1, 2});
  w.u(8, 120);    // general_level_idc
  w.flag(false);  // sub_layer_profile_present_flag[0]
  w.flag(true);   // sub_layer_level_present_flag[0]
  w.u(2 * 7, 0);  // reserved_zero_2bits[1..7]
  w.u(8, 90);     // sub_layer_level_idc[0]
  w.flag(true);   // vps_sub_layer_ordering_info_present_flag
  w.ue(2);        // sub-layer 0: vps_max_dec_pic_buffering_minus1, vps_max_num_reorder_pics, latency
  w.ue(1);
  w.ue(0);
  w.ue(4);  // sub-layer 1
  w.ue(2);
  w.ue(0);
  w.u(6, 0);       // vps_max_layer_id
  w.ue(1);         // vps_num_layer_sets_minus1 (two layer sets, so two hrd_parameters() are allowed)
  w.flag(true);    // layer_id_included_flag[1][0]
  w.flag(true);    // vps_timing_info_present_flag
  w.u(32, 1001);   // vps_num_units_in_tick
  w.u(32, 60000);  // vps_time_scale
  w.flag(true);    // vps_poc_proportional_to_timing_flag
  w.ue(0);         // vps_num_ticks_poc_diff_one_minus1
  w.ue(2);         // vps_num_hrd_parameters
  w.ue(0);         // hrd_layer_set_idx[0]; cprms_present_flag[0] inferred 1
  w.flag(true);    // nal_hrd_parameters_present_flag
  w.flag(false);   // vcl_hrd_parameters_present_flag
  w.flag(false);   // sub_pic_hrd_params_present_flag
  w.u(4, 1);       // bit_rate_scale
  w.u(4, 0);       // cpb_size_scale
  w.u(5, 23);      // initial_cpb_removal_delay_length_minus1
  w.u(5, 23);      // au_cpb_removal_delay_length_minus1
  w.u(5, 23);      // dpb_output_delay_length_minus1
  for (uint32_t i = 0; i < 2; ++i) {
    w.flag(true);   // fixed_pic_rate_general_flag
    w.ue(i);        // elemental_duration_in_tc_minus1
    w.ue(0);        // cpb_cnt_minus1 (low_delay_hrd_flag inferred 0)
    w.ue(100 + i);  // nal: bit_rate_value_minus1[0]
    w.ue(200 + i);  // cpb_size_value_minus1[0]
    w.flag(false);  // cbr_flag
  }
  w.ue(1);        // hrd_layer_set_idx[1]
  w.flag(false);  // cprms_present_flag[1]: common information inherited from entry 0 (7.4.3.1)
  for (uint32_t i = 0; i < 2; ++i) {
    w.flag(true);   // fixed_pic_rate_general_flag
    w.ue(3);        // elemental_duration_in_tc_minus1
    w.ue(0);        // cpb_cnt_minus1
    w.ue(300 + i);  // nal: bit_rate_value_minus1[0] (present because inherited nal_hrd_parameters_present_flag)
    w.ue(400 + i);
    w.flag(true);
  }
  w.flag(false);  // vps_extension_flag
  w.trailing();
  std::vector<uint8_t> nal = {0x40, 0x01};  // VPS_NUT, TemporalId 0
  const std::vector<uint8_t> body = w.bytes();
  nal.insert(nal.end(), body.begin(), body.end());

  SyntaxNodeBuffer tree;
  const h265::Vps vps = parse_vps_nal(nal, &tree);
  CHECK(vps.vps_video_parameter_set_id == 3);
  CHECK(vps.vps_max_sub_layers_minus1 == 1);
  CHECK(vps.vps_reserved_0xffff_16bits == 0xFFFF);
  CHECK(vps.profile_tier_level.general_level_idc == 120);
  CHECK(vps.profile_tier_level.sub_layer_level_idc[0] == 90);
  CHECK(vps.profile_tier_level.profile_name == "Main");
  CHECK(vps.sub_layer_ordering_info[0].max_dec_pic_buffering_minus1 == 2);
  CHECK(vps.sub_layer_ordering_info[1].max_dec_pic_buffering_minus1 == 4);
  CHECK(vps.sub_layer_ordering_info[1].max_num_reorder_pics == 2);
  CHECK(vps.vps_num_layer_sets_minus1 == 1);
  CHECK(vps.hrd[1].hrd_layer_set_idx == 1);
  CHECK(vps.vps_num_units_in_tick == 1001);
  CHECK(vps.vps_time_scale == 60000);
  CHECK(vps.vps_poc_proportional_to_timing_flag);
  CHECK(vps.frame_rate == Catch::Approx(59.94).epsilon(1e-4));
  REQUIRE(vps.hrd.size() == 2);
  CHECK(vps.hrd[0].cprms_present_flag);
  CHECK(vps.hrd[0].hrd_parameters.sub_layer[1].elemental_duration_in_tc_minus1 == 1);
  CHECK(vps.hrd[0].hrd_parameters.sub_layer[1].nal.cpb.at(0).bit_rate_value_minus1 == 101);
  CHECK_FALSE(vps.hrd[1].cprms_present_flag);
  CHECK(vps.hrd[1].hrd_parameters.nal_hrd_parameters_present_flag);  // inherited
  CHECK(vps.hrd[1].hrd_parameters.bit_rate_scale == 1);
  CHECK(vps.hrd[1].hrd_parameters.sub_layer[0].elemental_duration_in_tc_minus1 == 3);
  CHECK(vps.hrd[1].hrd_parameters.sub_layer[1].nal.cpb.at(0).bit_rate_value_minus1 == 301);
  CHECK(vps.hrd[1].hrd_parameters.sub_layer[1].nal.cpb.at(0).BitRate == 302U << 7U);
  CHECK(vps.hrd[1].hrd_parameters.sub_layer[1].nal.cpb.at(0).cbr_flag);
  CHECK_FALSE(vps.vps_extension_flag);
  CHECK_FALSE(vps.vps_extension_opaque);
  CHECK(vps.span.bit_offset == 16);
  CHECK(vps.span.bit_length == w.size_bits());
  const SyntaxNodeData* root = find_child(tree.root(), "video_parameter_set_rbsp");
  REQUIRE(root != nullptr);
  CHECK(find_child(root, "profile_tier_level") != nullptr);
  CHECK(find_child(root, "rbsp_trailing_bits") != nullptr);
}

// ---- hand-built SPS with SCC extensions and an inference check ---------------------------------------------

namespace {

// A minimal 64x64 4:2:0 SPS with sps_scc_extension() (palette with initializers) and sps_extension_4bits data.
std::vector<uint8_t> scc_sps_rbsp(bool with_extension_data) {
  BitWriter w;
  w.u(4, 0);     // sps_video_parameter_set_id
  w.u(3, 0);     // sps_max_sub_layers_minus1
  w.flag(true);  // sps_temporal_id_nesting_flag
  write_general_profile(w, 9, {9});
  w.u(8, 90);  // level 3
  w.ue(0);     // sps_seq_parameter_set_id
  w.ue(1);     // chroma_format_idc
  w.ue(64);
  w.ue(64);
  w.flag(false);  // conformance_window_flag
  w.ue(2);        // bit_depth_luma_minus8 (10 bit)
  w.ue(0);
  w.ue(4);       // log2_max_pic_order_cnt_lsb_minus4
  w.flag(true);  // sps_sub_layer_ordering_info_present_flag
  w.ue(4);
  w.ue(2);
  w.ue(0);
  w.ue(0);  // log2_min_luma_coding_block_size_minus3
  w.ue(1);  // log2_diff_max_min_luma_coding_block_size => CtbLog2SizeY 4
  w.ue(0);
  w.ue(2);
  w.ue(1);
  w.ue(1);
  w.flag(false);  // scaling_list_enabled_flag
  w.flag(false);  // amp
  w.flag(false);  // sao
  w.flag(false);  // pcm
  w.ue(0);        // num_short_term_ref_pic_sets
  w.flag(true);   // long_term_ref_pics_present_flag
  w.ue(1);        // num_long_term_ref_pics_sps
  w.u(8, 200);    // lt_ref_pic_poc_lsb_sps[0] u(v), v = 8
  w.flag(true);   // used_by_curr_pic_lt_sps_flag[0]
  w.flag(false);  // sps_temporal_mvp_enabled_flag
  w.flag(false);  // strong_intra_smoothing_enabled_flag
  w.flag(false);  // vui_parameters_present_flag
  w.flag(true);   // sps_extension_present_flag
  w.flag(false);  // range
  w.flag(false);  // multilayer
  w.flag(false);  // 3d
  w.flag(true);   // scc
  w.u(4, with_extension_data ? 1 : 0);
  // sps_scc_extension
  w.flag(true);   // sps_curr_pic_ref_enabled_flag
  w.flag(true);   // palette_mode_enabled_flag
  w.ue(8);        // palette_max_size
  w.ue(4);        // delta_palette_max_predictor_size
  w.flag(true);   // sps_palette_predictor_initializers_present_flag
  w.ue(1);        // sps_num_palette_predictor_initializers_minus1 => 2 entries
  w.u(10, 1023);  // comp 0 (BitDepthY = 10)
  w.u(10, 512);
  w.u(8, 128);  // comp 1 (BitDepthC = 8)
  w.u(8, 64);
  w.u(8, 1);  // comp 2
  w.u(8, 2);
  w.u(2, 2);     // motion_vector_resolution_control_idc
  w.flag(true);  // intra_boundary_filtering_disabled_flag
  if (with_extension_data) {
    w.u(8, 0xAB);  // sps_extension_data_flag bits, opaque
  }
  w.trailing();
  return w.bytes();
}

}  // namespace

TEST_CASE("h265_sps_scc_extension: palette initializers use BitDepthY/C widths; extension data stays opaque",
          "[syntax][h265][fast]") {
  for (const bool with_data : {false, true}) {
    const std::vector<uint8_t> data = scc_sps_rbsp(with_data);
    BitReader br(data);
    SyntaxBuilder b(br);
    const h265::Sps sps = h265::parse_sps(br, b);
    CHECK(sps.profile_tier_level.profile_name == "SCC (general_profile_idc 9)");
    CHECK(sps.BitDepthY == 10);
    CHECK(sps.CtbLog2SizeY == 4);
    CHECK(sps.PicWidthInCtbsY == 4);
    CHECK(sps.num_long_term_ref_pics_sps == 1);
    CHECK(sps.lt_ref_pic_poc_lsb_sps[0] == 200);
    CHECK(sps.used_by_curr_pic_lt_sps_flag[0]);
    CHECK(sps.sps_scc_extension_flag);
    const h265::SpsSccExtension& e = sps.scc_extension;
    CHECK(e.sps_curr_pic_ref_enabled_flag);
    CHECK(e.palette_max_size == 8);
    CHECK(e.PaletteMaxPredictorSize == 12);
    REQUIRE(e.sps_palette_predictor_initializer[0].size() == 2);
    CHECK(e.sps_palette_predictor_initializer[0][0] == 1023);
    CHECK(e.sps_palette_predictor_initializer[0][1] == 512);
    CHECK(e.sps_palette_predictor_initializer[1][0] == 128);
    CHECK(e.sps_palette_predictor_initializer[2][1] == 2);
    CHECK(e.motion_vector_resolution_control_idc == 2);
    CHECK(e.intra_boundary_filtering_disabled_flag);
    CHECK(sps.opaque_tail == with_data);
    CHECK(br.bit_pos() == data.size() * 8);
    const SyntaxNodeBuffer tree = b.finish();
    const SyntaxNodeData* root = tree.root()->child(0);
    REQUIRE(root != nullptr);
    CHECK(root->name == "seq_parameter_set_rbsp");
    if (with_data) {
      const SyntaxNodeData* opaque = find_child(root, "sps_extension_data_flag");
      REQUIRE(opaque != nullptr);
      CHECK(opaque->kind == ovt::syntax::ValueKind::Bytes);
      CHECK(root->diagnostic_count() == 1);
      CHECK(root->first_diag->code == "SYN-H265-0004");
    } else {
      CHECK(root->diagnostic_count() == 0);
      REQUIRE(find_child(root, "rbsp_trailing_bits") != nullptr);
    }
  }
}

TEST_CASE("h265_sps: nuh_layer_id > 0 keeps the SPS opaque after sps_video_parameter_set_id", "[syntax][h265][fast]") {
  const std::vector<uint8_t> data = scc_sps_rbsp(false);
  BitReader br(data);
  SyntaxBuilder b(br);
  const h265::Sps sps = h265::parse_sps(br, b, 1);
  CHECK(sps.opaque_tail);
  CHECK(br.bit_pos() == data.size() * 8);
  const SyntaxNodeBuffer tree = b.finish();
  CHECK(tree.root()->child(0)->child_count() == 2);  // sps_video_parameter_set_id + the opaque node
}

TEST_CASE("h265_sps: out-of-range sps_seq_parameter_set_id and chroma_format_idc throw SYN-H265-0007",
          "[syntax][h265][fast]") {
  for (const auto& [sps_id, chroma] : {std::pair{16U, 1U}, std::pair{0U, 4U}}) {
    BitWriter w;
    w.u(4, 0);
    w.u(3, 0);
    w.flag(true);
    write_general_profile(w, 1, {1, 2});
    w.u(8, 90);
    w.ue(sps_id);
    w.ue(chroma);
    w.ue(64);
    w.ue(64);
    const std::vector<uint8_t> data = w.bytes();
    BitReader br(data);
    SyntaxBuilder b(br);
    bool threw = false;
    try {
      (void)h265::parse_sps(br, b);
    } catch (const ParseError& e) {
      threw = true;
      CHECK(e.code == "SYN-H265-0007");
    }
    CHECK(threw);
    const SyntaxNodeBuffer tree = b.finish();  // partial tree survives
    CHECK(tree.root()->child_count() == 1);
  }
}
