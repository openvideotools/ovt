// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 nal_unit_header(), profile_tier_level() and hrd_parameters() tests (chapter 7 §7.3.5 rows 1, 3 and 6;
// WI-1.112). Every structure is hand-assembled with the BitWriter and checked against values computed from the
// H.265 text; the corpus acceptance tests live in test_syntax_h265_parameter_sets.cpp.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "h265_test_util.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/hrd_parameters.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/profile_tier_level.hpp"
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
using ovt::test::h265::write_general_profile;
namespace h265 = ovt::syntax::h265;

}  // namespace

// ---- row 1: nal_unit_header -----------------------------------------------------------------------------------

TEST_CASE("h265_nal_header: fields, classification and the SYN-H265-0001 ParseError", "[syntax][h265][fast]") {
  SECTION("VPS header 40 01") {
    const std::array<uint8_t, 2> data = {0x40, 0x01};
    BitReader br(data);
    SyntaxBuilder b(br);
    const h265::NalHeader h = h265::parse_nal_header(br, b);
    CHECK(h.nal_unit_type == h265::kVpsNut);
    CHECK(h.nuh_layer_id == 0);
    CHECK(h.nuh_temporal_id_plus1 == 1);
    CHECK(h.TemporalId == 0);
    CHECK_FALSE(h.is_vcl);
    CHECK(h.span.bit_offset == 0);
    CHECK(h.span.bit_length == 16);
    const SyntaxNodeBuffer tree = b.finish();
    const SyntaxNodeData* hdr = tree.root()->child(0);
    REQUIRE(hdr != nullptr);
    CHECK(hdr->name == "nal_unit_header");
    CHECK(hdr->child(1)->name == "nal_unit_type");
    CHECK(hdr->child(1)->enum_name == "VPS_NUT");
    CHECK(hdr->child(1)->descriptor == "u(6)");
  }
  SECTION("IDR_W_RADL with TemporalId 0, layer 2") {
    const std::array<uint8_t, 2> data = {0x26, 0x11};  // type 19, layer 2, tid+1 = 1
    BitReader br(data);
    SyntaxBuilder b(br);
    const h265::NalHeader h = h265::parse_nal_header(br, b);
    CHECK(h.nal_unit_type == 19);
    CHECK(h.nuh_layer_id == 2);
    CHECK(h.is_vcl);
    CHECK(h.is_irap);
    CHECK(h.is_idr);
    CHECK_FALSE(h.is_cra);
    CHECK_FALSE(h.is_sub_layer_non_ref);
  }
  SECTION("TRAIL_N is a sub-layer non-reference picture; RASL_R is RASL") {
    for (const auto& [type, non_ref, rasl] : {std::tuple{0U, true, false}, std::tuple{9U, false, true},
                                              std::tuple{14U, true, false}, std::tuple{21U, false, false}}) {
      const std::array<uint8_t, 2> data = {static_cast<uint8_t>(type << 1), 0x01};
      BitReader br(data);
      SyntaxBuilder b(br);
      const h265::NalHeader h = h265::parse_nal_header(br, b);
      CHECK(h.is_sub_layer_non_ref == non_ref);
      CHECK(h.is_rasl == rasl);
      CHECK(h.is_cra == (type == 21));
    }
  }
  SECTION("nuh_temporal_id_plus1 == 0 throws SYN-H265-0001") {
    const std::array<uint8_t, 2> data = {0x40, 0x00};
    BitReader br(data);
    SyntaxBuilder b(br);
    bool threw = false;
    try {
      (void)h265::parse_nal_header(br, b);
    } catch (const ParseError& e) {
      threw = true;
      CHECK(e.code == "SYN-H265-0001");
    }
    CHECK(threw);
  }
  SECTION("Table 7-1 names") {
    CHECK(h265::nal_unit_type_name(1) == "TRAIL_R");
    CHECK(h265::nal_unit_type_name(10) == "RSV_VCL_N10");
    CHECK(h265::nal_unit_type_name(39) == "PREFIX_SEI_NUT");
    CHECK(h265::nal_unit_type_name(62) == "UNSPEC62");
  }
}

// ---- row 3: profile_tier_level with sub-layers -------------------------------------------------------------

TEST_CASE("h265_profile_tier_level: Main 10 general block, two sub-layers, reserved_zero_2bits",
          "[syntax][h265][fast]") {
  BitWriter w;
  write_general_profile(w, 2, {1, 2});
  w.u(8, 93);  // general_level_idc 3.1
  // maxNumSubLayersMinus1 = 2: present flags for i = 0, 1
  w.flag(true);   // sub_layer_profile_present_flag[0]
  w.flag(true);   // sub_layer_level_present_flag[0]
  w.flag(false);  // sub_layer_profile_present_flag[1]
  w.flag(true);   // sub_layer_level_present_flag[1]
  for (unsigned i = 2; i < 8; ++i) {
    w.u(2, 0);  // reserved_zero_2bits[i]
  }
  write_general_profile(w, 1, {1, 2});  // sub_layer_profile[0]
  w.u(8, 90);                           // sub_layer_level_idc[0] = 3
  w.u(8, 60);                           // sub_layer_level_idc[1] = 2
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  const h265::ProfileTierLevel ptl = h265::parse_profile_tier_level(br, b, true, 2);
  CHECK(ptl.general.profile_idc == 2);
  CHECK(ptl.general.profile_compatibility_flag[1]);
  CHECK(ptl.general.profile_compatibility_flag[2]);
  CHECK_FALSE(ptl.general.profile_compatibility_flag[3]);
  CHECK(ptl.general.progressive_source_flag);
  CHECK_FALSE(ptl.general.interlaced_source_flag);
  CHECK(ptl.general.non_packed_constraint_flag);
  CHECK_FALSE(ptl.general.frame_only_constraint_flag);
  CHECK(ptl.general.inbld_flag_present);
  CHECK(ptl.general_level_idc == 93);
  CHECK(ptl.level == 3.1);
  CHECK(ptl.profile_name == "Main 10");
  CHECK(ptl.sub_layer_profile_present_flag[0]);
  CHECK_FALSE(ptl.sub_layer_profile_present_flag[1]);
  CHECK(ptl.sub_layer[0].profile_idc == 1);
  CHECK(ptl.sub_layer_level_idc[0] == 90);
  CHECK(ptl.sub_layer_level_idc[1] == 60);
  CHECK(br.bit_pos() == w.size_bits());
  CHECK(ptl.span.bit_length == w.size_bits());
  const SyntaxNodeBuffer tree = b.finish();
  const SyntaxNodeData* node = tree.root()->child(0);
  REQUIRE(node != nullptr);
  CHECK(node->name == "profile_tier_level");
  CHECK(node->spec_ref == "H.265 7.3.3");
  const SyntaxNodeData* name = find_child(node, "profile_name");
  REQUIRE(name != nullptr);
  CHECK(name->derived == "Main 10");
}

TEST_CASE("h265_profile_tier_level: RExt constraint flags select the Annex A sub-profile", "[syntax][h265][fast]") {
  h265::ProfileInfo p;
  p.profile_idc = 4;
  p.max_12bit_constraint_flag = true;
  p.lower_bit_rate_constraint_flag = true;
  CHECK(h265::profile_name(p) == "Main 4:4:4 12");
  p.max_10bit_constraint_flag = true;
  p.max_422chroma_constraint_flag = true;
  CHECK(h265::profile_name(p) == "Main 4:2:2 10");
  p.max_8bit_constraint_flag = true;
  p.max_420chroma_constraint_flag = true;
  p.max_monochrome_constraint_flag = true;
  CHECK(h265::profile_name(p) == "Monochrome");
  p.intra_constraint_flag = true;
  p.max_monochrome_constraint_flag = false;
  CHECK(h265::profile_name(p) == "Main Intra");
  p.one_picture_only_constraint_flag = true;
  p.max_420chroma_constraint_flag = false;
  p.max_422chroma_constraint_flag = false;
  CHECK(h265::profile_name(p) == "Main 4:4:4 Still Picture");
  p.profile_idc = 9;
  p.one_picture_only_constraint_flag = false;
  p.intra_constraint_flag = false;
  CHECK(h265::profile_name(p) == "Screen-Extended Main 4:4:4");
  p.profile_idc = 2;
  p.one_picture_only_constraint_flag = true;
  CHECK(h265::profile_name(p) == "Main 10 Still Picture");
}

// ---- row 6: hrd_parameters ------------------------------------------------------------------------------------

TEST_CASE("h265_hrd_parameters: common info, sub-picture params and two CPBs with derived BitRate/CpbSize",
          "[syntax][h265][fast]") {
  BitWriter w;
  w.flag(true);   // nal_hrd_parameters_present_flag
  w.flag(false);  // vcl
  w.flag(true);   // sub_pic_hrd_params_present_flag
  w.u(8, 98);     // tick_divisor_minus2
  w.u(5, 23);     // du_cpb_removal_delay_increment_length_minus1
  w.flag(true);   // sub_pic_cpb_params_in_pic_timing_sei_flag
  w.u(5, 5);      // dpb_output_delay_du_length_minus1
  w.u(4, 2);      // bit_rate_scale
  w.u(4, 3);      // cpb_size_scale
  w.u(4, 1);      // cpb_size_du_scale
  w.u(5, 23);     // initial_cpb_removal_delay_length_minus1
  w.u(5, 15);     // au_cpb_removal_delay_length_minus1
  w.u(5, 5);      // dpb_output_delay_length_minus1
  // sub-layer 0
  w.flag(false);  // fixed_pic_rate_general_flag
  w.flag(true);   // fixed_pic_rate_within_cvs_flag
  w.ue(0);        // elemental_duration_in_tc_minus1
  w.ue(1);        // cpb_cnt_minus1 (low_delay_hrd_flag not present => 0)
  // nal sub_layer_hrd_parameters, 2 CPBs
  w.ue(999);   // bit_rate_value_minus1[0]
  w.ue(1999);  // cpb_size_value_minus1[0]
  w.ue(7);     // cpb_size_du_value_minus1[0]
  w.ue(8);     // bit_rate_du_value_minus1[0]
  w.flag(true);
  w.ue(1);  // bit_rate_value_minus1[1]
  w.ue(0);
  w.ue(0);
  w.ue(0);
  w.flag(false);
  const std::vector<uint8_t> data = w.bytes();
  BitReader br(data);
  SyntaxBuilder b(br);
  const h265::HrdParameters h = h265::parse_hrd_parameters(br, b, true, 0);
  CHECK(h.nal_hrd_parameters_present_flag);
  CHECK(h.sub_pic_hrd_params_present_flag);
  CHECK(h.tick_divisor_minus2 == 98);
  CHECK(h.au_cpb_removal_delay_length_minus1 == 15);
  CHECK(h.bit_rate_scale == 2);
  CHECK(h.cpb_size_scale == 3);
  CHECK(h.cpb_size_du_scale == 1);
  const h265::SubLayerHrdInfo& sl = h.sub_layer[0];
  CHECK_FALSE(sl.fixed_pic_rate_general_flag);
  CHECK(sl.fixed_pic_rate_within_cvs_flag);
  CHECK_FALSE(sl.low_delay_hrd_flag);
  CHECK(sl.cpb_cnt_minus1 == 1);
  REQUIRE(sl.nal.cpb.size() == 2);
  CHECK(sl.vcl.cpb.empty());
  CHECK(sl.nal.cpb[0].bit_rate_value_minus1 == 999);
  CHECK(sl.nal.cpb[0].BitRate == 1000U << 8U);  // (999 + 1) << (6 + 2)
  CHECK(sl.nal.cpb[0].CpbSize == 2000U << 7U);  // (1999 + 1) << (4 + 3)
  CHECK(sl.nal.cpb[0].cpb_size_du_value_minus1 == 7);
  CHECK(sl.nal.cpb[0].cbr_flag);
  CHECK(sl.nal.cpb[1].BitRate == 2U << 8U);
  CHECK_FALSE(sl.nal.cpb[1].cbr_flag);
  CHECK(br.bit_pos() == w.size_bits());
  const SyntaxNodeBuffer tree = b.finish();
  const SyntaxNodeData* node = tree.root()->child(0);
  REQUIRE(node != nullptr);
  const SyntaxNodeData* nal = find_child(node, "nal_sub_layer_hrd_parameters");
  REQUIRE(nal != nullptr);
  CHECK(nal->array_index == 0);
  CHECK(nal->spec_ref == "H.265 E.2.3");
}
