// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the H.264 ActiveSets (chapter 7 §7.3.8, WI-1.111a): activation, the "replace before a picture /
// identical within it" rule, the re-parse of a PPS parsed before its SPS, and the Table 7-2 scaling-list fall-back.
// The parameter sets are written bit by bit in the test; the expected values follow from H.264 §7.3.2 / §7.4.2.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/active_sets.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace {

namespace h264 = ovt::syntax::h264;
using ovt::syntax::Severity;

h264::NalHeader make_nal(unsigned type, unsigned ref_idc) {
  h264::NalHeader n;
  n.nal_unit_type = type;
  n.nal_ref_idc = ref_idc;
  n.IdrPicFlag = type == 5;
  return n;
}

bool has_set_code(const std::vector<h264::SetDiagnostic>& d, std::string_view code, Severity sev) {
  return std::ranges::any_of(d, [&](const auto& x) { return x.code == code && x.severity == sev; });
}

// Writes RBSP bits for hand-made parameter sets (no emulation prevention needed for these short payloads; the
// writers below never produce 0x000000..03).
class BitWriter {
 public:
  void u(uint32_t bits, uint64_t v) {
    for (uint32_t i = bits; i-- > 0;) {
      bit(((v >> i) & 1U) != 0);
    }
  }
  void ue(uint32_t v) {
    const uint64_t code = uint64_t{v} + 1;
    uint32_t len = 0;
    while ((code >> (len + 1)) != 0) {
      ++len;
    }
    u(len, 0);
    u(len + 1, code);
  }
  void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * int64_t{v})); }
  void trailing() {
    bit(true);
    while (n_ % 8 != 0) {
      bit(false);
    }
  }
  [[nodiscard]] const std::vector<uint8_t>& bytes() const { return out_; }

 private:
  void bit(bool b) {
    if (n_ % 8 == 0) {
      out_.push_back(0);
    }
    if (b) {
      out_.back() = static_cast<uint8_t>(out_.back() | (0x80U >> (n_ % 8)));
    }
    ++n_;
  }
  std::vector<uint8_t> out_;
  uint64_t n_ = 0;
};

// A High-profile SPS (profile_idc 100) with the given chroma_format_idc; 2x2 macroblocks, frame_mbs_only.
std::vector<uint8_t> sps_bytes(uint32_t sps_id, uint32_t chroma_format_idc, uint32_t level_idc = 40) {
  BitWriter w;
  w.u(8, 0x67);  // nal_ref_idc 3, nal_unit_type 7
  w.u(8, 100);
  w.u(8, 0);
  w.u(8, level_idc);
  w.ue(sps_id);
  w.ue(chroma_format_idc);
  if (chroma_format_idc == 3) {
    w.u(1, 0);  // separate_colour_plane_flag
  }
  w.ue(0);    // bit_depth_luma_minus8
  w.ue(0);    // bit_depth_chroma_minus8
  w.u(1, 0);  // qpprime_y_zero_transform_bypass_flag
  w.u(1, 0);  // seq_scaling_matrix_present_flag
  w.ue(0);    // log2_max_frame_num_minus4
  w.ue(0);    // pic_order_cnt_type
  w.ue(0);    // log2_max_pic_order_cnt_lsb_minus4
  w.ue(1);    // max_num_ref_frames
  w.u(1, 0);  // gaps_in_frame_num_value_allowed_flag
  w.ue(1);    // pic_width_in_mbs_minus1
  w.ue(1);    // pic_height_in_map_units_minus1
  w.u(1, 1);  // frame_mbs_only_flag
  w.u(1, 1);  // direct_8x8_inference_flag
  w.u(1, 0);  // frame_cropping_flag
  w.u(1, 0);  // vui_parameters_present_flag
  w.trailing();
  return w.bytes();
}

// A PPS with transform_8x8_mode_flag 1; `qp` is pic_init_qp_minus26. `matrix_lists` 0 writes no scaling matrix,
// otherwise pic_scaling_matrix_present_flag 1 and that many pic_scaling_list_present_flag zeros (8 for
// chroma_format_idc != 3, 12 for 4:4:4).
std::vector<uint8_t> pps_bytes(uint32_t pps_id, uint32_t sps_id, int32_t qp = 0, uint32_t matrix_lists = 0,
                               int32_t second_chroma_qp_index_offset = 0) {
  BitWriter w;
  w.u(8, 0x68);  // nal_ref_idc 3, nal_unit_type 8
  w.ue(pps_id);
  w.ue(sps_id);
  w.u(1, 0);                            // entropy_coding_mode_flag
  w.u(1, 0);                            // bottom_field_pic_order_in_frame_present_flag
  w.ue(0);                              // num_slice_groups_minus1
  w.ue(0);                              // num_ref_idx_l0_default_active_minus1
  w.ue(0);                              // num_ref_idx_l1_default_active_minus1
  w.u(1, 0);                            // weighted_pred_flag
  w.u(2, 0);                            // weighted_bipred_idc
  w.se(qp);                             // pic_init_qp_minus26
  w.se(0);                              // pic_init_qs_minus26
  w.se(0);                              // chroma_qp_index_offset
  w.u(1, 1);                            // deblocking_filter_control_present_flag
  w.u(1, 0);                            // constrained_intra_pred_flag
  w.u(1, 0);                            // redundant_pic_cnt_present_flag
  w.u(1, 1);                            // transform_8x8_mode_flag
  w.u(1, matrix_lists != 0 ? 1U : 0U);  // pic_scaling_matrix_present_flag
  w.u(matrix_lists, 0);                 // pic_scaling_list_present_flag[i]
  w.se(second_chroma_qp_index_offset);
  w.trailing();
  return w.bytes();
}

h264::Sps parse_sps_bytes(const std::vector<uint8_t>& rbsp) {
  ovt::BitReader br(rbsp);
  ovt::syntax::SyntaxBuilder b(br);
  (void)h264::parse_nal_header(br, b);
  return h264::parse_sps(br, b);
}

h264::Pps parse_pps_bytes(const std::vector<uint8_t>& rbsp, const h264::SpsTable& t) {
  ovt::BitReader br(rbsp);
  ovt::syntax::SyntaxBuilder b(br);
  (void)h264::parse_nal_header(br, b);
  return h264::parse_pps(br, b, t);
}

h264::SliceHeader slice_with_pps(uint32_t pps_id) {
  h264::SliceHeader s;
  s.pic_parameter_set_id = pps_id;
  return s;
}

}  // namespace

// ---- ActiveSets (§7.3.8) -------------------------------------------------------------------------------------------

TEST_CASE("h264_active_sets: activation resolves the PPS and its SPS and records their units", "[index][h264][fast]") {
  h264::ActiveSets sets;
  std::vector<h264::SetDiagnostic> diags;
  const auto sps = sps_bytes(0, 1);
  sets.put_sps(parse_sps_bytes(sps), sps, 3, diags);
  const auto pps = pps_bytes(5, 0);
  sets.put_pps(parse_pps_bytes(pps, sets.parameter_sets(diags).sps), pps, 4, diags);
  const h264::ParameterSets& ps = sets.parameter_sets(diags);
  REQUIRE(ps.pps.find(5) != nullptr);
  REQUIRE(ps.sps.find(0) != nullptr);
  const auto a = sets.activate(make_nal(5, 3), slice_with_pps(5), true, diags);
  REQUIRE(a.has_value());
  CHECK(a->pps_unit == 4);
  CHECK(a->sps_unit == 3);
  CHECK_FALSE(a->subset);
  CHECK(a->scaling->flat);
  CHECK(sets.picture_open());
  CHECK(diags.empty());
}

TEST_CASE("h264_active_sets: missing PPS or SPS is Error SYN-H264-0010", "[index][h264][fast]") {
  h264::ActiveSets sets;
  std::vector<h264::SetDiagnostic> diags;
  CHECK_FALSE(sets.activate(make_nal(1, 2), slice_with_pps(0), true, diags).has_value());
  CHECK(has_set_code(diags, "SYN-H264-0010", Severity::Error));
  diags.clear();
  const auto pps = pps_bytes(0, 7);
  sets.put_pps(parse_pps_bytes(pps, {}), pps, 0, diags);
  CHECK_FALSE(sets.activate(make_nal(1, 2), slice_with_pps(0), true, diags).has_value());
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].message == "slice references PPS 0 whose SPS 7 is missing");
  diags.clear();
  // nal_unit_type 20 looks in the subset SPS table only.
  const auto sps = sps_bytes(7, 1);
  sets.put_sps(parse_sps_bytes(sps), sps, 1, diags);
  CHECK_FALSE(sets.activate(make_nal(20, 2), slice_with_pps(0), true, diags).has_value());
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].message == "slice references PPS 0 whose subset SPS 7 is missing");
  diags.clear();
  sets.put_subset_sps(parse_sps_bytes(sps), sps, 2, diags);
  const auto a = sets.activate(make_nal(20, 2), slice_with_pps(0), true, diags);
  REQUIRE(a.has_value());
  CHECK(a->subset);
  CHECK(a->sps_unit == 2);
}

TEST_CASE("h264_active_sets: a PPS parsed before its SPS is re-parsed when the SPS arrives", "[index][h264][fast]") {
  h264::ActiveSets sets;
  std::vector<h264::SetDiagnostic> diags;
  // A PPS for a 4:4:4 SPS: 12 pic_scaling_list_present_flag zeros, then second_chroma_qp_index_offset = -6. Parsed
  // provisionally it reads 8 flags (chroma_format_idc != 3 assumed), so the offset comes out wrong (108).
  const auto pps = pps_bytes(1, 2, 0, 12, -6);
  const h264::Pps provisional = parse_pps_bytes(pps, {});
  REQUIRE(provisional.provisional);
  REQUIRE(provisional.num_scaling_lists == 8);
  CHECK(provisional.second_chroma_qp_index_offset == 108);
  sets.put_pps(provisional, pps, 0, diags);
  const uint64_t g0 = sets.pps_generation(1);
  CHECK(sets.parameter_sets(diags).pps.find(1)->provisional);  // no SPS yet: nothing to re-parse
  const auto sps = sps_bytes(2, 3);
  sets.put_sps(parse_sps_bytes(sps), sps, 1, diags);
  const h264::Pps* p = sets.parameter_sets(diags).pps.find(1);
  REQUIRE(p != nullptr);
  CHECK_FALSE(p->provisional);
  CHECK(p->num_scaling_lists == 12);
  CHECK(p->second_chroma_qp_index_offset == -6);
  CHECK(sets.pps_generation(1) != g0);
  // A repeat of the same SPS bytes changes nothing.
  const uint64_t g1 = sets.pps_generation(1);
  sets.put_sps(parse_sps_bytes(sps), sps, 2, diags);
  (void)sets.parameter_sets(diags);
  CHECK(sets.pps_generation(1) == g1);
  CHECK(diags.empty());
}

TEST_CASE("h264_active_sets: a re-parse that fails keeps the earlier PPS and reports the error",
          "[index][h264][fast]") {
  h264::ActiveSets sets;
  std::vector<h264::SetDiagnostic> diags;
  // A PPS written for 4:2:0 (8 list flags): against a 4:4:4 SPS the re-parse reads 4 more flags, finds list 8
  // "present" and runs out of bits.
  const auto pps = pps_bytes(0, 0, 0, 8, 0);
  const h264::Pps provisional = parse_pps_bytes(pps, {});
  REQUIRE(provisional.provisional);
  sets.put_pps(provisional, pps, 0, diags);
  const auto sps = sps_bytes(0, 3);
  sets.put_sps(parse_sps_bytes(sps), sps, 1, diags);
  const h264::Pps* p = sets.parameter_sets(diags).pps.find(0);
  REQUIRE(p != nullptr);
  CHECK(p->provisional);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].severity == Severity::Error);
  // One attempt per SPS generation.
  diags.clear();
  (void)sets.parameter_sets(diags);
  CHECK(diags.empty());
  // An SPS change (4:2:0 now) re-parses again, successfully.
  const auto sps420 = sps_bytes(0, 1);
  sets.put_sps(parse_sps_bytes(sps420), sps420, 2, diags);
  CHECK_FALSE(sets.parameter_sets(diags).pps.find(0)->provisional);
  CHECK(sets.pps(0)->num_scaling_lists == 8);
  CHECK(diags.empty());
}

TEST_CASE("h264_active_sets: replace before a picture, identical within it", "[index][h264][fast]") {
  h264::ActiveSets sets;
  std::vector<h264::SetDiagnostic> diags;
  const auto sps = sps_bytes(0, 1);
  sets.put_sps(parse_sps_bytes(sps), sps, 0, diags);
  const auto pps_a = pps_bytes(0, 0, 0);
  const auto pps_b = pps_bytes(0, 0, -4);
  sets.put_pps(parse_pps_bytes(pps_a, sets.parameter_sets(diags).sps), pps_a, 1, diags);
  // Before any picture: the new PPS replaces the entry.
  sets.put_pps(parse_pps_bytes(pps_b, sets.parameter_sets(diags).sps), pps_b, 2, diags);
  CHECK(sets.pps(0)->pic_init_qp_minus26 == -4);
  CHECK(diags.empty());
  auto a = sets.activate(make_nal(5, 3), slice_with_pps(0), true, diags);
  REQUIRE(a.has_value());
  // Inside the picture: an identical copy is fine, a different one is an Error and waits for the next picture.
  sets.put_pps(parse_pps_bytes(pps_b, sets.parameter_sets(diags).sps), pps_b, 3, diags);
  CHECK(diags.empty());
  sets.put_pps(parse_pps_bytes(pps_a, sets.parameter_sets(diags).sps), pps_a, 4, diags);
  REQUIRE(has_set_code(diags, "SYN-H264-0021", Severity::Error));
  CHECK(diags[0].message == "PPS 0 changed within a picture");
  CHECK(sets.pps(0)->pic_init_qp_minus26 == -4);
  // The activation keeps its PPS alive after the replacement.
  const auto held = a->pps;
  a = sets.activate(make_nal(1, 2), slice_with_pps(0), true, diags);
  REQUIRE(a.has_value());
  CHECK(a->pps->pic_init_qp_minus26 == 0);
  CHECK(a->pps_unit == 4);
  CHECK(held->pic_init_qp_minus26 == -4);
}

TEST_CASE("h264_scaling: Table 7-2 fall-back rules A and B", "[index][h264][fast]") {
  h264::Sps sps;
  h264::Pps pps;
  // Neither has a matrix: Flat_4x4_16 / Flat_8x8_16.
  auto m = h264::resolve_scaling_matrices(sps, pps);
  CHECK(m.flat);
  CHECK(m.list4x4[5][15] == 16);
  CHECK(m.list8x8[3][63] == 16);
  // SPS matrix with only list 1 present: rule A gives Default_4x4_Intra for list 0, list 1 as sent, list 2 copies
  // list 1, list 3 Default_4x4_Inter, lists 4, 5 copy it; 8x8: 6 and 7 defaults, 8..11 copy 6/7 (H.264 Table 7-2).
  sps.seq_scaling_matrix_present_flag = true;
  sps.scaling_list[1].present = true;
  sps.scaling_list[1].list.fill(7);
  m = h264::resolve_scaling_matrices(sps, pps);
  CHECK_FALSE(m.flat);
  CHECK(m.list4x4[0][0] == 6);
  CHECK(m.list4x4[0][15] == 42);
  CHECK(m.list4x4[1][3] == 7);
  CHECK(m.list4x4[2][3] == 7);
  CHECK(m.list4x4[3][0] == 10);
  CHECK(m.list4x4[5][15] == 34);
  CHECK(m.list8x8[0][63] == 42);
  CHECK(m.list8x8[1][0] == 9);
  CHECK(m.list8x8[4][63] == 42);
  CHECK(m.list8x8[5][0] == 9);
  // useDefaultScalingMatrixFlag selects the default of the list's class.
  sps.scaling_list[3].present = true;
  sps.scaling_list[3].UseDefaultScalingMatrixFlag = true;
  sps.scaling_list[0].present = true;
  sps.scaling_list[0].list.fill(3);
  m = h264::resolve_scaling_matrices(sps, pps);
  CHECK(m.list4x4[0][0] == 3);
  CHECK(m.list4x4[3][15] == 34);
  // PPS matrix with list 4 present and SPS lists present: rule B takes lists 0, 3, 6, 7 from the SPS level.
  pps.pic_scaling_matrix_present_flag = true;
  pps.scaling_list[4].present = true;
  pps.scaling_list[4].list.fill(9);
  m = h264::resolve_scaling_matrices(sps, pps);
  CHECK(m.list4x4[0][0] == 3);    // SPS level list 0
  CHECK(m.list4x4[1][0] == 3);    // copies PPS list 0
  CHECK(m.list4x4[3][15] == 34);  // SPS level list 3
  CHECK(m.list4x4[4][0] == 9);
  CHECK(m.list4x4[5][0] == 9);
  // Same PPS without SPS matrices: rule A for the PPS lists.
  sps.seq_scaling_matrix_present_flag = false;
  m = h264::resolve_scaling_matrices(sps, pps);
  CHECK(m.list4x4[0][0] == 6);
  CHECK(m.list4x4[3][0] == 10);
  CHECK(m.list4x4[4][0] == 9);
}
