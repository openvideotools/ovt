// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 NAL header, VPS, PTL, SPS and PPS parser tests (chapter 7 §7.3.5 rows 1–9, WI-1.112).
//
// Expected values of the corpus tests are hard-coded from the recorded oracle outputs committed as goldens:
//   tests/golden/h265/jctvc_SLIST_A_Sony_5.trace_headers.json.gz   (FFmpeg trace_headers, acceptance test 1)
//   tests/golden/h265/jctvc_TILES_B_Cisco_1.gstparse.json.gz       (GStreamer gst_parse_oracle, acceptance test 2)
//   tests/golden/h265/jctvc_Bitdepth_A_RExt_Sony_1.gstparse.json.gz (GStreamer gst_parse_oracle, acceptance test 3)
// The streams themselves are fetched by tools/fetch_corpora.py; the [needs-corpus] cases skip when absent.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "h265_test_util.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/profile_tier_level.hpp"
#include "ovt/cxx/syntax/h265/scaling_list.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/h265/vui.hpp"
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
using ovt::test::h265::BitWriter;
using ovt::test::h265::find_child;
using ovt::test::h265::parse_nal;
using ovt::test::h265::write_general_profile;
namespace h265 = ovt::syntax::h265;

struct ParameterSets {
  std::vector<uint8_t> vps;  // the NAL unit bytes (header included, EPBs still present)
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
};

ParameterSets first_parameter_sets(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ParameterSets ps;
  ovt::syntax::scan_annexb(std::span<const uint8_t>(data), 0, ovt::syntax::kNoContainerSample,
                           [&ps](const ovt::syntax::ScannedUnit& u) {
                             if (u.bytes.size() < 2) {
                               return;
                             }
                             const auto type = static_cast<uint8_t>((u.bytes[0] >> 1) & 0x3F);
                             std::vector<uint8_t>* dst = type == h265::kVpsNut   ? &ps.vps
                                                         : type == h265::kSpsNut ? &ps.sps
                                                         : type == h265::kPpsNut ? &ps.pps
                                                                                 : nullptr;
                             if (dst != nullptr && dst->empty()) {
                               dst->assign(u.bytes.begin(), u.bytes.end());
                             }
                           });
  REQUIRE_FALSE(ps.vps.empty());
  REQUIRE_FALSE(ps.sps.empty());
  REQUIRE_FALSE(ps.pps.empty());
  return ps;
}

h265::Vps parse_vps_nal(const std::vector<uint8_t>& nal, SyntaxNodeBuffer* tree = nullptr) {
  return parse_nal(
      nal, [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) { return h265::parse_vps(br, b); }, tree);
}
h265::Sps parse_sps_nal(const std::vector<uint8_t>& nal, SyntaxNodeBuffer* tree = nullptr) {
  return parse_nal(
      nal,
      [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader& h) { return h265::parse_sps(br, b, h.nuh_layer_id); },
      tree);
}
h265::Pps parse_pps_nal(const std::vector<uint8_t>& nal, SyntaxNodeBuffer* tree = nullptr) {
  return parse_nal(
      nal, [](BitReader& br, SyntaxBuilder& b, const h265::NalHeader&) { return h265::parse_pps(br, b); }, tree);
}

}  // namespace

// ---- row 2: video_parameter_set_rbsp (hand-built) -------------------------------------------------------------

TEST_CASE("h265_vps: two sub-layers, timing info, two hrd_parameters() entries and the derived frame rate",
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

// ---- acceptance test 1: SLIST_A_Sony_5 (scaling lists) --------------------------------------------------------

TEST_CASE("h265_ps_slist_a: PTL, scaling_list_enabled_flag and scaling_list_dc_coef_minus8 match trace_headers",
          "[syntax][h265][fast][needs-corpus]") {
  const ParameterSets ps = first_parameter_sets(ovt::test::corpus_or_skip("jctvc_SLIST_A_Sony_5"));
  SyntaxNodeBuffer vps_tree;
  const h265::Vps vps = parse_vps_nal(ps.vps, &vps_tree);
  CHECK(vps.vps_video_parameter_set_id == 0);
  CHECK(vps.vps_max_sub_layers_minus1 == 0);
  CHECK(vps.vps_temporal_id_nesting_flag);
  CHECK(vps.vps_reserved_0xffff_16bits == 0xFFFF);
  CHECK(vps.profile_tier_level.general.profile_idc == 1);
  CHECK(vps.profile_tier_level.general_level_idc == 120);
  CHECK(vps.sub_layer_ordering_info[0].max_dec_pic_buffering_minus1 == 4);
  CHECK(vps.sub_layer_ordering_info[0].max_num_reorder_pics == 3);
  CHECK(vps.vps_num_layer_sets_minus1 == 0);
  CHECK_FALSE(vps.vps_timing_info_present_flag);
  CHECK_FALSE(vps.vps_extension_flag);
  CHECK(vps_tree.root()->diagnostic_count() == 0);

  SyntaxNodeBuffer sps_tree;
  const h265::Sps sps = parse_sps_nal(ps.sps, &sps_tree);
  // profile_tier_level: general_profile_idc = 1, compatibility flags 1 and 2, level_idc 120 (4.0)
  const h265::ProfileInfo& g = sps.profile_tier_level.general;
  CHECK(g.profile_space == 0);
  CHECK_FALSE(g.tier_flag);
  CHECK(g.profile_idc == 1);
  CHECK(g.profile_compatibility_flag[1]);
  CHECK(g.profile_compatibility_flag[2]);
  CHECK_FALSE(g.profile_compatibility_flag[0]);
  CHECK_FALSE(g.profile_compatibility_flag[3]);
  CHECK_FALSE(g.progressive_source_flag);
  CHECK_FALSE(g.one_picture_only_constraint_flag);
  CHECK(g.inbld_flag_present);
  CHECK_FALSE(g.inbld_flag);
  CHECK(sps.profile_tier_level.general_level_idc == 120);
  CHECK(sps.profile_tier_level.profile_name == "Main");
  CHECK(sps.profile_tier_level.level == 4.0);
  CHECK(sps.pic_width_in_luma_samples == 832);
  CHECK(sps.pic_height_in_luma_samples == 480);
  CHECK(sps.conformance_window_flag);
  CHECK(sps.log2_max_pic_order_cnt_lsb_minus4 == 4);
  CHECK(sps.sub_layer_ordering_info[0].max_dec_pic_buffering_minus1 == 4);
  CHECK(sps.max_transform_hierarchy_depth_inter == 2);
  // scaling lists
  CHECK(sps.scaling_list_enabled_flag);
  CHECK(sps.sps_scaling_list_data_present_flag);
  const h265::ScalingListData& sl = sps.scaling_list;
  CHECK(sl.scaling_list_pred_mode_flag[0][0]);
  CHECK_FALSE(sl.scaling_list_pred_mode_flag[0][1]);
  CHECK(sl.scaling_list_pred_matrix_id_delta[0][1] == 1);
  CHECK_FALSE(sl.scaling_list_pred_mode_flag[1][4]);
  // scaling_list_dc_coef_minus8[sizeId - 2][matrixId] from the trace; [0][2] and [1][3] are predicted from the
  // default list (scaling_list_pred_matrix_id_delta = 0) and therefore inferred 8
  CHECK(sl.scaling_list_dc_coef_minus8[0][0] == -7);
  CHECK(sl.scaling_list_dc_coef_minus8[0][1] == -6);
  CHECK(sl.scaling_list_dc_coef_minus8[0][2] == 8);
  CHECK(sl.scaling_list_dc_coef_minus8[0][3] == -7);
  CHECK(sl.scaling_list_dc_coef_minus8[0][4] == 119);
  CHECK(sl.scaling_list_dc_coef_minus8[0][5] == 247);
  CHECK(sl.scaling_list_dc_coef_minus8[1][0] == -7);
  CHECK(sl.scaling_list_dc_coef_minus8[1][3] == 8);
  // ScalingList values accumulated from the trace's scaling_list_delta_coeff
  const std::array<uint8_t, 16> list00 = {1, 255, 255, 1, 1, 1, 255, 255, 255, 255, 1, 1, 1, 255, 255, 1};
  for (size_t i = 0; i < 16; ++i) {
    CHECK(sl.ScalingList[0][0][i] == list00[i]);
    CHECK(sl.ScalingList[0][1][i] == list00[i]);  // predicted from matrixId 0
    CHECK(sl.ScalingList[0][3][i] == 16);         // default
  }
  const std::array<uint8_t, 16> list04 = {1, 65, 17, 129, 81, 33, 193, 145, 97, 49, 209, 161, 113, 225, 177, 241};
  for (size_t i = 0; i < 16; ++i) {
    CHECK(sl.ScalingList[0][4][i] == list04[i]);
  }
  const std::array<uint8_t, 16> list20 = {1, 9, 2, 17, 10, 3, 25, 18, 11, 4, 33, 26, 19, 12, 5, 41};
  const std::array<uint8_t, 16> list30 = {127, 119, 126, 111, 118, 125, 103, 110, 117, 124, 95, 102, 109, 116, 123, 87};
  for (size_t i = 0; i < 16; ++i) {
    CHECK(sl.ScalingList[2][0][i] == list20[i]);
    CHECK(sl.ScalingList[3][0][i] == list30[i]);
  }
  CHECK(sl.ScalingList[1][0][0] == 6);
  CHECK(sl.scaling_factor(2, 0, 0, 0) == 1);  // dc -7 + 8
  CHECK(sl.scaling_factor(3, 0, 0, 0) == 1);
  // the rest of the SPS
  CHECK(sps.amp_enabled_flag);
  CHECK_FALSE(sps.sample_adaptive_offset_enabled_flag);
  CHECK_FALSE(sps.pcm_enabled_flag);
  CHECK(sps.num_short_term_ref_pic_sets == 11);
  REQUIRE(sps.st_ref_pic_set.size() == 11);
  CHECK(sps.st_ref_pic_set[0].num_negative_pics == 4);
  CHECK(sps.st_ref_pic_set[0].DeltaPocS0[0] == -8);
  CHECK(sps.st_ref_pic_set[0].DeltaPocS0[1] == -10);
  CHECK(sps.st_ref_pic_set[0].DeltaPocS0[2] == -12);
  CHECK(sps.st_ref_pic_set[0].DeltaPocS0[3] == -16);
  CHECK(sps.st_ref_pic_set[1].inter_ref_pic_set_prediction_flag);
  CHECK(sps.st_ref_pic_set[1].abs_delta_rps_minus1 == 3);
  CHECK(sps.st_ref_pic_set[8].num_negative_pics == 1);
  CHECK_FALSE(sps.long_term_ref_pics_present_flag);
  CHECK(sps.sps_temporal_mvp_enabled_flag);
  CHECK(sps.strong_intra_smoothing_enabled_flag);
  CHECK_FALSE(sps.vui_parameters_present_flag);
  CHECK_FALSE(sps.sps_extension_present_flag);
  CHECK(sps.CtbSizeY == 64);
  CHECK(sps.PicWidthInCtbsY == 13);
  CHECK(sps.PicHeightInCtbsY == 8);
  CHECK(sps_tree.root()->diagnostic_count() == 0);
  CHECK(sps.span.bit_length + 16 == sps_tree.root()->span.bit_length);

  SyntaxNodeBuffer pps_tree;
  const h265::Pps pps = parse_pps_nal(ps.pps, &pps_tree);
  CHECK(pps.pps_pic_parameter_set_id == 0);
  CHECK(pps.sign_data_hiding_enabled_flag);
  CHECK(pps.cabac_init_present_flag);
  CHECK(pps.num_ref_idx_l0_default_active_minus1 == 1);
  CHECK(pps.num_ref_idx_l1_default_active_minus1 == 1);
  CHECK(pps.init_qp_minus26 == 0);
  CHECK(pps.transform_skip_enabled_flag);
  CHECK_FALSE(pps.tiles_enabled_flag);
  CHECK(pps.pps_loop_filter_across_slices_enabled_flag);
  CHECK_FALSE(pps.pps_scaling_list_data_present_flag);
  CHECK_FALSE(pps.pps_extension_present_flag);
  CHECK(pps_tree.root()->diagnostic_count() == 0);

  // determinism (§0.7.5): a second parse yields byte-identical JSON
  SyntaxNodeBuffer again;
  (void)parse_sps_nal(ps.sps, &again);
  CHECK(ovt::syntax::to_json(again) == ovt::syntax::to_json(sps_tree));
}

// ---- acceptance test 2: TILES_B_Cisco_1 (tiles) --------------------------------------------------------------

TEST_CASE("h265_ps_tiles_b: colBd/rowBd match GStreamer column_width_minus1 and CtbAddrRsToTs is a permutation",
          "[syntax][h265][fast][needs-corpus]") {
  const ParameterSets ps = first_parameter_sets(ovt::test::corpus_or_skip("jctvc_TILES_B_Cisco_1"));
  const h265::Sps sps = parse_sps_nal(ps.sps);
  CHECK(sps.pic_width_in_luma_samples == 1920);
  CHECK(sps.pic_height_in_luma_samples == 1080);
  CHECK(sps.CtbLog2SizeY == 6);
  CHECK(sps.PicWidthInCtbsY == 30);
  CHECK(sps.PicHeightInCtbsY == 17);
  CHECK(sps.profile_tier_level.general_level_idc == 123);
  SyntaxNodeBuffer pps_tree;
  const h265::Pps pps = parse_pps_nal(ps.pps, &pps_tree);
  CHECK(pps.tiles_enabled_flag);
  CHECK(pps.num_tile_columns_minus1 == 4);
  CHECK(pps.num_tile_rows_minus1 == 4);
  CHECK_FALSE(pps.uniform_spacing_flag);
  CHECK(pps.column_width_minus1 == std::vector<uint32_t>{3, 9, 4, 5});
  CHECK(pps.row_height_minus1 == std::vector<uint32_t>{7, 2, 1, 2});
  CHECK(pps.loop_filter_across_tiles_enabled_flag);
  CHECK_FALSE(pps.pps_loop_filter_across_slices_enabled_flag);
  CHECK(pps.init_qp_minus26 == 11);
  CHECK(pps.deblocking_filter_control_present_flag);
  CHECK_FALSE(pps.pps_deblocking_filter_disabled_flag);
  CHECK(pps.pps_beta_offset_div2 == 4);
  CHECK(pps.pps_tc_offset_div2 == 4);
  CHECK(pps_tree.root()->diagnostic_count() == 0);

  const h265::ActivePps active = h265::activate_pps(pps, sps);
  CHECK(active.issues.empty());
  CHECK(active.colBd == std::vector<uint32_t>{0, 4, 14, 19, 25, 30});
  CHECK(active.rowBd == std::vector<uint32_t>{0, 8, 11, 13, 16, 17});
  CHECK(active.ColumnWidthInLumaSamples == std::vector<uint32_t>{256, 640, 320, 384, 320});
  CHECK(active.RowHeightInLumaSamples == std::vector<uint32_t>{512, 192, 128, 192, 64});
  REQUIRE(active.CtbAddrRsToTs.size() == size_t{30} * 17);
  std::vector<uint32_t> sorted = active.CtbAddrRsToTs;
  std::sort(sorted.begin(), sorted.end());
  std::vector<uint32_t> identity(sorted.size());
  std::iota(identity.begin(), identity.end(), 0);
  CHECK(sorted == identity);
  for (uint32_t rs = 0; rs < active.CtbAddrRsToTs.size(); ++rs) {
    CHECK(active.CtbAddrTsToRs[active.CtbAddrRsToTs[rs]] == rs);
  }
  // first tile: 4 x 8 CTBs in raster rows of 30, so raster address 30 (second row) is tile-scan address 4
  CHECK(active.CtbAddrRsToTs[0] == 0);
  CHECK(active.CtbAddrRsToTs[30] == 4);
  CHECK(active.CtbAddrRsToTs[4] == 32);  // first CTB of tile 1 follows the 32 CTBs of tile 0
  CHECK(active.TileId[0] == 0);
  CHECK(active.TileId[32] == 1);
  CHECK(active.TileId[active.CtbAddrRsToTs[30 * 17 - 1]] == 24);
  CHECK(active.Log2MinCuQpDeltaSize == 6);
  CHECK(active.SliceQpY_default == 37);
}

// ---- acceptance test 3: Bitdepth_A_RExt_Sony_1 (range extension) + SCC (skipped: no scc-tagged corpus) --------

TEST_CASE("h265_ps_bitdepth_a: sps_range_extension and pps_range_extension fields match gst_parse_oracle",
          "[syntax][h265][fast][needs-corpus]") {
  const ParameterSets ps = first_parameter_sets(ovt::test::corpus_or_skip("jctvc_Bitdepth_A_RExt_Sony_1"));
  SyntaxNodeBuffer sps_tree;
  const h265::Sps sps = parse_sps_nal(ps.sps, &sps_tree);
  const h265::ProfileInfo& g = sps.profile_tier_level.general;
  CHECK(g.profile_idc == 4);
  CHECK(g.profile_compatibility_flag[4]);
  CHECK(g.max_12bit_constraint_flag);
  CHECK_FALSE(g.max_10bit_constraint_flag);
  CHECK_FALSE(g.max_8bit_constraint_flag);
  CHECK_FALSE(g.max_422chroma_constraint_flag);
  CHECK_FALSE(g.max_420chroma_constraint_flag);
  CHECK_FALSE(g.max_monochrome_constraint_flag);
  CHECK_FALSE(g.intra_constraint_flag);
  CHECK(g.lower_bit_rate_constraint_flag);
  CHECK_FALSE(g.max_14bit_constraint_flag);
  CHECK(sps.profile_tier_level.profile_name == "Main 4:4:4 12");
  CHECK(sps.profile_tier_level.general_level_idc == 123);
  CHECK(sps.chroma_format_idc == 3);
  CHECK_FALSE(sps.separate_colour_plane_flag);
  CHECK(sps.bit_depth_luma_minus8 == 4);
  CHECK(sps.bit_depth_chroma_minus8 == 0);
  CHECK(sps.BitDepthY == 12);
  CHECK(sps.BitDepthC == 8);
  CHECK(sps.QpBdOffsetY == 24);
  CHECK(sps.ChromaArrayType == 3);
  CHECK(sps.SubWidthC == 1);
  CHECK(sps.conformance_window_flag);
  CHECK(sps.conformance_crop.w == 1920);
  CHECK(sps.conformance_crop.h == 1080);
  CHECK(sps.amp_enabled_flag);
  CHECK(sps.sample_adaptive_offset_enabled_flag);
  CHECK(sps.num_short_term_ref_pic_sets == 14);
  REQUIRE(sps.st_ref_pic_set.size() == 14);
  // DeltaPocS0 / NumDeltaPocs per set from the GStreamer golden (sets 1..3 and 5..12 are inter-predicted)
  const std::array<std::array<int32_t, 4>, 13> s0 = {{{-1, -5, -9, -13},
                                                      {-1, -2, -6, -10},
                                                      {-1, -3, -7, -11},
                                                      {-1, -4, -8, -12},
                                                      {-1, 0, 0, 0},
                                                      {-1, -2, 0, 0},
                                                      {-1, -2, -3, 0},
                                                      {-1, -2, -3, -4},
                                                      {-1, -2, -3, -5},
                                                      {-1, -2, -3, -6},
                                                      {-1, -2, -3, -7},
                                                      {-1, -2, -4, -8},
                                                      {-1, -2, -5, -9}}};
  const std::array<uint32_t, 14> num_delta = {4, 4, 4, 4, 1, 2, 3, 4, 4, 4, 4, 4, 4, 0};
  const std::array<bool, 14> inter = {false, true, true, true, false, true, true,
                                      true,  true, true, true, true,  true, false};
  for (size_t i = 0; i < 14; ++i) {
    const h265::StRefPicSet& s = sps.st_ref_pic_set[i];
    CHECK(s.inter_ref_pic_set_prediction_flag == inter[i]);
    CHECK(s.NumDeltaPocs == num_delta[i]);
    CHECK(s.NumPositivePics == 0);
    for (uint32_t j = 0; j < s.NumNegativePics; ++j) {
      CHECK(s.DeltaPocS0[j] == s0[i][j]);
      CHECK(s.UsedByCurrPicS0[j]);
    }
  }
  CHECK(sps.st_ref_pic_set[1].delta_rps_sign);
  CHECK(sps.st_ref_pic_set[1].abs_delta_rps_minus1 == 0);
  CHECK_FALSE(sps.vui_parameters_present_flag);
  // extensions
  CHECK(sps.sps_extension_present_flag);
  CHECK(sps.sps_range_extension_flag);
  CHECK_FALSE(sps.sps_multilayer_extension_flag);
  CHECK_FALSE(sps.sps_3d_extension_flag);
  CHECK_FALSE(sps.sps_scc_extension_flag);
  CHECK(sps.sps_extension_4bits == 0);
  const h265::SpsRangeExtension& r = sps.range_extension;
  CHECK(r.transform_skip_rotation_enabled_flag);
  CHECK(r.transform_skip_context_enabled_flag);
  CHECK(r.implicit_rdpcm_enabled_flag);
  CHECK(r.explicit_rdpcm_enabled_flag);
  CHECK_FALSE(r.extended_precision_processing_flag);
  CHECK_FALSE(r.intra_smoothing_disabled_flag);
  CHECK(r.high_precision_offsets_enabled_flag);
  CHECK(r.persistent_rice_adaptation_enabled_flag);
  CHECK_FALSE(r.cabac_bypass_alignment_enabled_flag);
  CHECK(sps.WpOffsetBdShiftY == 0);  // high_precision_offsets_enabled_flag
  CHECK(sps.WpOffsetHalfRangeY == 1U << 11U);
  CHECK(sps.CoeffMaxY == 32767);
  CHECK_FALSE(sps.opaque_tail);
  CHECK(sps_tree.root()->diagnostic_count() == 0);
  const SyntaxNodeData* ext = find_child(sps_tree.root()->child(1), "sps_range_extension");
  REQUIRE(ext != nullptr);
  CHECK(ext->spec_ref == "H.265 7.3.2.2.2");
  CHECK(ext->child_count() == 9);

  SyntaxNodeBuffer pps_tree;
  const h265::Pps pps = parse_pps_nal(ps.pps, &pps_tree);
  CHECK(pps.cabac_init_present_flag);
  CHECK(pps.sign_data_hiding_enabled_flag);
  CHECK(pps.num_ref_idx_l0_default_active_minus1 == 3);
  CHECK(pps.num_ref_idx_l1_default_active_minus1 == 3);
  CHECK(pps.transform_skip_enabled_flag);
  CHECK(pps.pps_loop_filter_across_slices_enabled_flag);
  CHECK(pps.pps_extension_present_flag);
  CHECK(pps.pps_range_extension_flag);
  CHECK_FALSE(pps.pps_scc_extension_flag);
  CHECK(pps.range_extension.log2_max_transform_skip_block_size_minus2 == 0);
  CHECK(pps.range_extension.cross_component_prediction_enabled_flag);
  CHECK_FALSE(pps.range_extension.chroma_qp_offset_list_enabled_flag);
  CHECK(pps.range_extension.log2_sao_offset_scale_luma == 0);
  CHECK(pps_tree.root()->diagnostic_count() == 0);
  const h265::ActivePps active = h265::activate_pps(pps, sps);
  CHECK(active.issues.empty());
  CHECK(active.colBd == std::vector<uint32_t>{0, 30});
  CHECK(active.CtbAddrRsToTs[100] == 100);

  // The SCC half of acceptance test 3 (first `scc`-tagged stream, nightly) is skipped: the corpus manifest has no
  // scc-tagged entry yet; h265_sps_scc_extension below covers the parser on a hand-built SPS.
}

// ---- hand-built SPS/PPS with SCC extensions and an inference check ---------------------------------------------

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

TEST_CASE("h265_sps: log2_min_luma_coding_block_size_minus3 beyond 3 throws SYN-H265-0007 before any shift",
          "[syntax][h265][fast]") {
  BitWriter w;
  w.u(4, 0);
  w.u(3, 0);
  w.flag(true);
  write_general_profile(w, 1, {1, 2});
  w.u(8, 90);
  w.ue(0);        // sps_seq_parameter_set_id
  w.ue(1);        // chroma_format_idc
  w.ue(64);       // pic_width_in_luma_samples
  w.ue(64);       // pic_height_in_luma_samples
  w.flag(false);  // conformance_window_flag
  w.ue(0);
  w.ue(0);
  w.ue(4);        // log2_max_pic_order_cnt_lsb_minus4
  w.flag(false);  // sps_sub_layer_ordering_info_present_flag
  w.ue(4);
  w.ue(2);
  w.ue(0);
  w.ue(29);  // log2_min_luma_coding_block_size_minus3: would make MinCbLog2SizeY a 32-bit shift count
  w.ue(0);
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  bool threw = false;
  try {
    (void)h265::parse_sps(br, b);
  } catch (const ParseError& e) {
    threw = true;
    CHECK(e.code == "SYN-H265-0007");
    CHECK(std::string_view{e.what()}.find("log2_min_luma_coding_block_size_minus3") != std::string_view::npos);
  }
  CHECK(threw);
}

TEST_CASE("h265_sps: sps_max_dec_pic_buffering_minus1 beyond 15 throws SYN-H265-0007", "[syntax][h265][fast]") {
  BitWriter w;
  w.u(4, 0);
  w.u(3, 0);
  w.flag(true);
  write_general_profile(w, 1, {1, 2});
  w.u(8, 90);
  w.ue(0);
  w.ue(1);
  w.ue(64);
  w.ue(64);
  w.flag(false);
  w.ue(0);
  w.ue(0);
  w.ue(4);
  w.flag(false);
  w.ue(16);  // sps_max_dec_pic_buffering_minus1: sizes the DPB
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  bool threw = false;
  try {
    (void)h265::parse_sps(br, b);
  } catch (const ParseError& e) {
    threw = true;
    CHECK(e.code == "SYN-H265-0007");
    CHECK(std::string_view{e.what()}.find("sps_max_dec_pic_buffering_minus1") != std::string_view::npos);
  }
  CHECK(threw);
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

TEST_CASE("h265_pps: pps_pic_parameter_set_id > 63 throws SYN-H265-0010; SCC extension and uniform tiles parse",
          "[syntax][h265][fast]") {
  SECTION("id out of range") {
    BitWriter w;
    w.ue(64);
    w.ue(0);
    const std::vector<uint8_t> data = w.bytes();
    BitReader br(data);
    SyntaxBuilder b(br);
    bool threw = false;
    try {
      (void)h265::parse_pps(br, b);
    } catch (const ParseError& e) {
      threw = true;
      CHECK(e.code == "SYN-H265-0010");
    }
    CHECK(threw);
  }
  SECTION("uniform tiles, deblocking offsets, chroma QP offset list and SCC palette initializers") {
    BitWriter w;
    w.ue(5);        // pps_pic_parameter_set_id
    w.ue(2);        // pps_seq_parameter_set_id
    w.flag(true);   // dependent_slice_segments_enabled_flag
    w.flag(false);  // output_flag_present_flag
    w.u(3, 2);      // num_extra_slice_header_bits
    w.flag(false);
    w.flag(true);  // cabac_init_present_flag
    w.ue(2);
    w.ue(0);
    w.se(-4);       // init_qp_minus26
    w.flag(false);  // constrained_intra_pred_flag
    w.flag(true);   // transform_skip_enabled_flag
    w.flag(true);   // cu_qp_delta_enabled_flag
    w.ue(1);        // diff_cu_qp_delta_depth
    w.se(-2);       // pps_cb_qp_offset
    w.se(3);        // pps_cr_qp_offset
    w.flag(true);   // pps_slice_chroma_qp_offsets_present_flag
    w.flag(true);   // weighted_pred_flag
    w.flag(false);  // weighted_bipred_flag
    w.flag(false);  // transquant_bypass_enabled_flag
    w.flag(true);   // tiles_enabled_flag
    w.flag(true);   // entropy_coding_sync_enabled_flag
    w.ue(2);        // num_tile_columns_minus1
    w.ue(1);        // num_tile_rows_minus1
    w.flag(true);   // uniform_spacing_flag
    w.flag(false);  // loop_filter_across_tiles_enabled_flag
    w.flag(true);   // pps_loop_filter_across_slices_enabled_flag
    w.flag(true);   // deblocking_filter_control_present_flag
    w.flag(true);   // deblocking_filter_override_enabled_flag
    w.flag(false);  // pps_deblocking_filter_disabled_flag
    w.se(-3);       // pps_beta_offset_div2
    w.se(6);        // pps_tc_offset_div2
    w.flag(false);  // pps_scaling_list_data_present_flag
    w.flag(true);   // lists_modification_present_flag
    w.ue(2);        // log2_parallel_merge_level_minus2
    w.flag(true);   // slice_segment_header_extension_present_flag
    w.flag(true);   // pps_extension_present_flag
    w.flag(true);   // range
    w.flag(false);  // multilayer
    w.flag(false);  // 3d
    w.flag(true);   // scc
    w.u(4, 0);
    // pps_range_extension
    w.ue(1);       // log2_max_transform_skip_block_size_minus2
    w.flag(true);  // cross_component_prediction_enabled_flag
    w.flag(true);  // chroma_qp_offset_list_enabled_flag
    w.ue(1);       // diff_cu_chroma_qp_offset_depth
    w.ue(1);       // chroma_qp_offset_list_len_minus1 => 2 entries
    w.se(-5);
    w.se(5);
    w.se(1);
    w.se(-1);
    w.ue(1);  // log2_sao_offset_scale_luma
    w.ue(0);
    // pps_scc_extension
    w.flag(true);  // pps_curr_pic_ref_enabled_flag
    w.flag(true);  // residual_adaptive_colour_transform_enabled_flag
    w.flag(true);  // pps_slice_act_qp_offsets_present_flag
    w.se(-5);
    w.se(0);
    w.se(-3);
    w.flag(true);  // pps_palette_predictor_initializers_present_flag
    w.ue(2);       // pps_num_palette_predictor_initializers
    w.flag(true);  // monochrome_palette_flag
    w.ue(2);       // luma_bit_depth_entry_minus8 => 10 bits
    w.u(10, 1000);
    w.u(10, 7);
    w.trailing();
    const std::vector<uint8_t> data = w.bytes();
    BitReader br(data);
    SyntaxBuilder b(br);
    const h265::Pps pps = h265::parse_pps(br, b);
    CHECK(br.bit_pos() == data.size() * 8);
    CHECK(pps.pps_pic_parameter_set_id == 5);
    CHECK(pps.pps_seq_parameter_set_id == 2);
    CHECK(pps.num_extra_slice_header_bits == 2);
    CHECK(pps.init_qp_minus26 == -4);
    CHECK(pps.SliceQpY_default == 22);
    CHECK(pps.diff_cu_qp_delta_depth == 1);
    CHECK(pps.pps_cb_qp_offset == -2);
    CHECK(pps.pps_cr_qp_offset == 3);
    CHECK(pps.num_tile_columns_minus1 == 2);
    CHECK(pps.num_tile_rows_minus1 == 1);
    CHECK(pps.uniform_spacing_flag);
    CHECK(pps.column_width_minus1.empty());
    CHECK_FALSE(pps.loop_filter_across_tiles_enabled_flag);
    CHECK(pps.pps_beta_offset_div2 == -3);
    CHECK(pps.pps_tc_offset_div2 == 6);
    CHECK(pps.Log2ParMrgLevel == 4);
    CHECK(pps.Log2MaxTransformSkipSize == 3);
    CHECK(pps.range_extension.cross_component_prediction_enabled_flag);
    CHECK(pps.range_extension.chroma_qp_offset_list_len_minus1 == 1);
    CHECK(pps.range_extension.cb_qp_offset_list[0] == -5);
    CHECK(pps.range_extension.cr_qp_offset_list[0] == 5);
    CHECK(pps.range_extension.cb_qp_offset_list[1] == 1);
    CHECK(pps.range_extension.cr_qp_offset_list[1] == -1);
    CHECK(pps.range_extension.log2_sao_offset_scale_luma == 1);
    const h265::PpsSccExtension& e = pps.scc_extension;
    CHECK(e.pps_curr_pic_ref_enabled_flag);
    CHECK(e.pps_slice_act_qp_offsets_present_flag);
    CHECK(e.pps_act_y_qp_offset_plus5 == -5);
    CHECK(e.pps_act_cr_qp_offset_plus3 == -3);
    CHECK(e.pps_num_palette_predictor_initializers == 2);
    CHECK(e.monochrome_palette_flag);
    CHECK(e.luma_bit_depth_entry_minus8 == 2);
    REQUIRE(e.pps_palette_predictor_initializer[0].size() == 2);
    CHECK(e.pps_palette_predictor_initializer[0][0] == 1000);
    CHECK(e.pps_palette_predictor_initializer[0][1] == 7);
    CHECK(e.pps_palette_predictor_initializer[1].empty());
    CHECK_FALSE(pps.opaque_tail);
    const SyntaxNodeBuffer tree = b.finish();
    CHECK(tree.root()->child(0)->diagnostic_count() == 0);

    // uniform spacing against the hand-built 64x64 SPS (4 x 4 CTBs of 16): 3 columns of 1, 2, 1 and 2 rows of 2
    const std::vector<uint8_t> sps_data = scc_sps_rbsp(false);
    BitReader sbr(sps_data);
    SyntaxBuilder sb(sbr);
    const h265::Sps sps = h265::parse_sps(sbr, sb);
    const h265::ActivePps active = h265::activate_pps(pps, sps);
    CHECK(active.issues.empty());
    CHECK(active.colBd == std::vector<uint32_t>{0, 1, 2, 4});
    CHECK(active.rowBd == std::vector<uint32_t>{0, 2, 4});
    CHECK(active.CtbAddrRsToTs == std::vector<uint32_t>{0, 2, 4, 5, 1, 3, 6, 7, 8, 10, 12, 13, 9, 11, 14, 15});
    CHECK(active.TileId == std::vector<uint32_t>{0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 5, 5, 5});
    CHECK(active.Log2MinCuQpDeltaSize == 3);
    CHECK(active.Log2MinCuChromaQpOffsetSize == 3);
  }
}

TEST_CASE("h265_pps: init_qp_minus26 outside [-74, 25] throws SYN-H265-0007 before SliceQpY is derived",
          "[syntax][h265][fast]") {
  BitWriter w;
  w.ue(0);        // pps_pic_parameter_set_id
  w.ue(0);        // pps_seq_parameter_set_id
  w.flag(false);  // dependent_slice_segments_enabled_flag
  w.flag(false);  // output_flag_present_flag
  w.u(3, 0);      // num_extra_slice_header_bits
  w.flag(false);  // sign_data_hiding_enabled_flag
  w.flag(false);  // cabac_init_present_flag
  w.ue(0);        // num_ref_idx_l0_default_active_minus1
  w.ue(0);        // num_ref_idx_l1_default_active_minus1
  w.se(26);       // init_qp_minus26: SliceQpY would be 52
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  bool threw = false;
  try {
    (void)h265::parse_pps(br, b);
  } catch (const ParseError& e) {
    threw = true;
    CHECK(e.code == "SYN-H265-0007");
    CHECK(std::string_view{e.what()}.find("init_qp_minus26") != std::string_view::npos);
  }
  CHECK(threw);
}

TEST_CASE("h265_pps: explicit tile sizes that do not fit the picture are clamped and reported",
          "[syntax][h265][fast]") {
  const std::vector<uint8_t> sps_data = scc_sps_rbsp(false);  // 4 x 4 CTBs
  BitReader sbr(sps_data);
  SyntaxBuilder sb(sbr);
  const h265::Sps sps = h265::parse_sps(sbr, sb);
  h265::Pps pps;
  pps.tiles_enabled_flag = true;
  pps.num_tile_columns_minus1 = 1;
  pps.uniform_spacing_flag = false;
  pps.column_width_minus1 = {5};  // 6 CTBs wide in a 4-CTB picture
  const h265::ActivePps active = h265::activate_pps(pps, sps);
  REQUIRE(active.issues.size() == 1);
  CHECK(active.issues[0].code == "SYN-H265-0007");
  CHECK(active.colBd == std::vector<uint32_t>{0, 3, 4});
  CHECK(active.CtbAddrRsToTs.size() == 16);
  h265::Pps too_many;
  too_many.tiles_enabled_flag = true;
  too_many.num_tile_columns_minus1 = 4;
  const h265::ActivePps bad = h265::activate_pps(too_many, sps);
  REQUIRE(bad.issues.size() == 1);
  CHECK(bad.colBd.empty());
  h265::Sps huge = sps;  // 65535x65535 in 16x16 CTBs: 16.7M CTBs, far beyond MaxLumaPs
  huge.PicWidthInCtbsY = 4096;
  huge.PicHeightInCtbsY = 4096;
  const h265::ActivePps oversize = h265::activate_pps(pps, huge);
  REQUIRE(oversize.issues.size() == 1);
  CHECK(oversize.issues[0].code == "SYN-H265-0007");
  CHECK(oversize.CtbAddrRsToTs.empty());
}
