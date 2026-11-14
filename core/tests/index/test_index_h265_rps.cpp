// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the H.265 index models (chapter 7 §7.3.8–§7.3.10.2, WI-1.115a): parameter-set activation, frame-boundary
// detection and POC with prevTid0Pic. The POC sequences are those of POC_A_Bossen_3, RPS_A_docomo_5 and
// NUT_A_ericsson_5 (LSBs from `trace_headers`); their first pictures equal HM's POC (hm-grid goldens), the rest follows
// from §8.3.1. WI-1.115b adds the RPS/DPB tests and the whole-stream comparison with the HM traces.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "index/h265_index.h"
#include "ovt/cxx/syntax/h265/active_sets.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace {

namespace h265 = ovt::syntax::h265;
namespace idx = ovt::index::h265;
using ovt::syntax::Severity;

// ---- builders -----------------------------------------------------------------------------------------------------

// The facts parse_nal_header() derives, for hand-made headers.
h265::NalHeader make_nal(unsigned type, unsigned tid = 0, unsigned layer = 0) {
  h265::NalHeader n;
  n.nal_unit_type = static_cast<uint8_t>(type);
  n.nuh_layer_id = static_cast<uint8_t>(layer);
  n.nuh_temporal_id_plus1 = static_cast<uint8_t>(tid + 1);
  n.TemporalId = static_cast<uint8_t>(tid);
  n.is_vcl = type < 32;
  n.is_irap = type >= 16 && type <= 23;
  n.is_idr = type == 19 || type == 20;
  n.is_bla = type >= 16 && type <= 18;
  n.is_cra = type == 21;
  n.is_rasl = type == 8 || type == 9;
  n.is_radl = type == 6 || type == 7;
  n.is_sub_layer_non_ref = type <= 14 && type % 2 == 0;
  return n;
}

h265::Sps make_sps(uint32_t max_lsb) {
  h265::Sps sps;
  sps.MaxPicOrderCntLsb = max_lsb;
  sps.pic_width_in_luma_samples = 64;
  sps.pic_height_in_luma_samples = 64;
  sps.CtbLog2SizeY = 4;
  sps.CtbSizeY = 16;
  sps.PicWidthInCtbsY = 4;
  sps.PicHeightInCtbsY = 4;
  sps.PicSizeInCtbsY = 16;
  sps.MaxDpbSizeMinus1 = 5;
  return sps;
}

}  // namespace

// ---- POC (§7.3.10.2) ---------------------------------------------------------------------------------------------

TEST_CASE("h265_poc: POC_A_Bossen_3 and NUT_A_ericsson_5 recorded sequences", "[index][h265][fast]") {
  SECTION("POC_A_Bossen_3: MaxPicOrderCntLsb 16, RADL_R before the trailing pictures, TRAIL_N skips prevTid0Pic") {
    // HM (hm-grid golden): picture 0 POC 0, picture 1 POC -7. The rest follows from §8.3.1.
    idx::PocState p;
    const h265::Sps sps = make_sps(16);
    struct In {
      unsigned nut;
      uint32_t lsb;
      int32_t expect;
    };
    const std::array<In, 5> seq = {{{19, 0, 0}, {7, 9, -7}, {1, 8, 8}, {0, 0, 16}, {0, 1, 1}}};
    for (const In& in : seq) {
      h265::SliceSegmentHeader sh;
      sh.slice_pic_order_cnt_lsb = in.lsb;
      CAPTURE(in.nut, in.lsb);
      CHECK(p.step(make_nal(in.nut), sh, sps).poc == in.expect);
    }
  }
  SECTION("NUT_A_ericsson_5: sub-layer non-reference pictures do not update prevTid0Pic") {
    // HM: pictures 0 and 1 have POC 0 and 10. IDR_N_LP, TRAIL_N, TRAIL_R, TSA_N (tid 1), TRAIL_R (tid 1), TRAIL_N (tid
    // 1).
    idx::PocState p;
    const h265::Sps sps = make_sps(256);
    struct In {
      unsigned nut;
      unsigned tid;
      uint32_t lsb;
      int32_t expect;
    };
    const std::array<In, 6> seq = {
        {{20, 0, 0, 0}, {0, 0, 10, 10}, {1, 0, 20, 20}, {2, 1, 30, 30}, {1, 1, 40, 40}, {0, 1, 50, 50}}};
    for (const In& in : seq) {
      h265::SliceSegmentHeader sh;
      sh.slice_pic_order_cnt_lsb = in.lsb;
      CAPTURE(in.nut, in.lsb);
      const idx::PocResult r = p.step(make_nal(in.nut, in.tid), sh, sps);
      CHECK(r.poc == in.expect);
      CHECK(r.updates_prev_tid0 == (in.tid == 0 && (in.nut > 14 || in.nut % 2 == 1)));
    }
  }
  SECTION("RPS_A_docomo_5: 0 8 4 2 1 3") {
    idx::PocState p;
    const h265::Sps sps = make_sps(256);
    const std::array<std::pair<unsigned, uint32_t>, 6> seq = {{{19, 0}, {1, 8}, {1, 4}, {1, 2}, {0, 1}, {0, 3}}};
    const std::vector<int32_t> expect = {0, 8, 4, 2, 1, 3};
    for (size_t i = 0; i < seq.size(); ++i) {
      h265::SliceSegmentHeader sh;
      sh.slice_pic_order_cnt_lsb = seq[i].second;
      CHECK(p.step(make_nal(seq[i].first), sh, sps).poc == expect[i]);
    }
  }
}

TEST_CASE("h265_poc: LSB wrap-around moves the MSB in both directions and survives negative POCs",
          "[index][h265][fast]") {
  idx::PocState p;
  const h265::Sps sps = make_sps(16);
  auto poc_of = [&](unsigned nut, uint32_t lsb) {
    h265::SliceSegmentHeader sh;
    sh.slice_pic_order_cnt_lsb = lsb;
    return p.step(make_nal(nut), sh, sps).poc;
  };
  CHECK(poc_of(19, 0) == 0);
  CHECK(poc_of(1, 6) == 6);
  CHECK(poc_of(1, 12) == 12);
  CHECK(poc_of(1, 2) == 18);   // lsb < prev and prev - lsb >= 8: MSB + 16
  CHECK(poc_of(1, 14) == 14);  // lsb > prev and lsb - prev > 8: MSB - 16
  CHECK(poc_of(1, 9) == 9);    // prev 14 (lsb 14, msb 0): 14 - 9 < 8, same MSB
  CHECK(poc_of(1, 1) == 17);
  // after an IDR the POC restarts; a negative POC reuses the floor modulo
  CHECK(poc_of(19, 0) == 0);
  CHECK(poc_of(1, 13) == -3);
  CHECK(poc_of(1, 11) == -5);  // prev -3: prevLsb 13, prevMsb -16; 11 < 13 but 13 - 11 < 8
  CHECK(poc_of(1, 3) == 3);    // prev -5: prevLsb 11, prevMsb -16; 3 < 11 and 11 - 3 = 8 >= 8: MSB + 16
}

TEST_CASE("h265_rap: NoRaslOutputFlag, RASL flags and EOS", "[index][h265][fast]") {
  const h265::Sps sps = make_sps(16);
  auto sh_lsb = [](uint32_t lsb) {
    h265::SliceSegmentHeader sh;
    sh.slice_pic_order_cnt_lsb = lsb;
    return sh;
  };
  SECTION("CRA first in the stream: NoRaslOutputFlag 1, its RASL pictures are skipped, RADL are not") {
    idx::PocState p;
    const idx::PocResult cra = p.step(make_nal(21), sh_lsb(6), sps);
    CHECK(cra.is_rap);
    CHECK(cra.no_rasl_output_flag);
    CHECK(cra.poc == 6);  // MSB 0, POC = lsb
    const idx::PocResult rasl = p.step(make_nal(9), sh_lsb(3), sps);
    CHECK(rasl.is_rasl);
    CHECK(rasl.rasl_skipped);
    CHECK_FALSE(rasl.is_rap);
    CHECK(rasl.poc == 3);
    CHECK_FALSE(rasl.updates_prev_tid0);
    const idx::PocResult radl = p.step(make_nal(7), sh_lsb(4), sps);
    CHECK_FALSE(radl.rasl_skipped);
    CHECK(radl.poc == 4);
  }
  SECTION("CRA in the middle: NoRaslOutputFlag 0, POC derived from prevTid0Pic, RASL decoded") {
    idx::PocState p;
    (void)p.step(make_nal(19), sh_lsb(0), sps);
    (void)p.step(make_nal(1), sh_lsb(4), sps);
    const idx::PocResult cra = p.step(make_nal(21), sh_lsb(8), sps);
    CHECK_FALSE(cra.no_rasl_output_flag);
    CHECK(cra.is_rap);
    CHECK(cra.poc == 8);
    const idx::PocResult rasl = p.step(make_nal(8), sh_lsb(6), sps);
    CHECK(rasl.is_rasl);
    CHECK_FALSE(rasl.rasl_skipped);
  }
  SECTION("the picture after an EOS restarts with MSB 0 and NoRaslOutputFlag 1") {
    idx::PocState p;
    (void)p.step(make_nal(19), sh_lsb(0), sps);
    (void)p.step(make_nal(1), sh_lsb(12), sps);
    (void)p.step(make_nal(1), sh_lsb(2), sps);  // POC 18
    p.on_eos();
    const idx::PocResult cra = p.step(make_nal(21), sh_lsb(10), sps);
    CHECK(cra.no_rasl_output_flag);
    CHECK(cra.poc == 10);
    CHECK(p.step(make_nal(8), sh_lsb(9), sps).rasl_skipped);
    // the next CRA is a normal one again
    (void)p.step(make_nal(1), sh_lsb(12), sps);
    CHECK_FALSE(p.step(make_nal(21), sh_lsb(14), sps).no_rasl_output_flag);
  }
  SECTION("HandleCraAsBlaFlag") {
    idx::PocState p;
    (void)p.step(make_nal(19), sh_lsb(0), sps);
    p.set_handle_cra_as_bla(true);
    const idx::PocResult cra = p.step(make_nal(21), sh_lsb(5), sps);
    CHECK(cra.no_rasl_output_flag);
    CHECK(cra.poc == 5);
  }
  SECTION("BLA always has NoRaslOutputFlag 1") {
    idx::PocState p;
    (void)p.step(make_nal(19), sh_lsb(0), sps);
    (void)p.step(make_nal(1), sh_lsb(7), sps);
    const idx::PocResult bla = p.step(make_nal(17), sh_lsb(9), sps);
    CHECK(bla.no_rasl_output_flag);
    CHECK(bla.poc == 9);
  }
}

// ---- frame boundary (§7.3.9) --------------------------------------------------------------------------------------

TEST_CASE("h265_frame_boundary: access unit and picture starts", "[index][h265][fast]") {
  idx::FrameBoundary fb;
  using idx::UnitRole;
  auto feed = [&](unsigned nut, bool first = false, unsigned layer = 0) {
    return fb.on_unit(make_nal(nut, 0, layer), first);
  };
  // VPS, SPS, PPS, first slice: nothing ends yet
  CHECK(feed(32).role == UnitRole::Preceding);
  CHECK_FALSE(feed(33).starts_access_unit);
  CHECK_FALSE(feed(34).starts_access_unit);
  CHECK(feed(19, true).role == UnitRole::FirstSlice);
  CHECK(feed(19, false).role == UnitRole::NextSlice);
  CHECK(feed(40).role == UnitRole::Following);  // suffix SEI belongs to the access unit
  // an AUD after the picture starts the next access unit
  const auto aud = feed(35);
  CHECK(aud.role == UnitRole::Preceding);
  CHECK(aud.starts_access_unit);
  CHECK(feed(1, true).role == UnitRole::FirstSlice);
  // prefix SEI after a VCL NAL starts an access unit once; a second one does not
  CHECK(feed(39).starts_access_unit);
  CHECK_FALSE(feed(39).starts_access_unit);
  CHECK(feed(1, true).role == UnitRole::FirstSlice);
  // NAL types 41..44 and 48..55 behave like prefix SEI; 45..47 and 56..63 do not start an access unit
  CHECK(feed(44).starts_access_unit);
  CHECK(feed(1, true).role == UnitRole::FirstSlice);
  CHECK_FALSE(feed(47).starts_access_unit);
  CHECK(feed(1, true).role == UnitRole::FirstSlice);
  CHECK_FALSE(feed(60).starts_access_unit);
  CHECK(feed(48).starts_access_unit);
  // two pictures back to back without a non-VCL unit in between
  CHECK(feed(1, true).role == UnitRole::FirstSlice);
  CHECK(feed(1, true).starts_access_unit);
  // EOS is reported and ends the picture: a slice that is not a first slice is then orphaned
  const auto eos = feed(36);
  CHECK(eos.is_eos);
  CHECK(eos.role == UnitRole::Following);
  const auto orphan = feed(1, false);
  CHECK(orphan.role == UnitRole::OrphanSlice);
  REQUIRE(orphan.diagnostics.size() == 1);
  CHECK(orphan.diagnostics[0].code == "SYN-H265-0060");
  CHECK(orphan.diagnostics[0].severity == Severity::Error);
  CHECK(feed(1, false).role == UnitRole::NextSlice);  // the synthetic frame takes it
}

TEST_CASE("h265_frame_boundary: pictures of other layers do not interrupt the base layer", "[index][h265][fast]") {
  idx::FrameBoundary fb;
  using idx::UnitRole;
  CHECK(fb.on_unit(make_nal(19, 0, 0), true).role == UnitRole::FirstSlice);
  const auto l1 = fb.on_unit(make_nal(1, 0, 1), true);
  CHECK(l1.role == UnitRole::FirstSlice);
  CHECK_FALSE(l1.starts_access_unit);
  CHECK(fb.on_unit(make_nal(19, 0, 0), false).role == UnitRole::NextSlice);
  CHECK(fb.on_unit(make_nal(1, 0, 1), false).role == UnitRole::NextSlice);
  // a continuation on a layer that has no open picture is an orphan, while the base layer is unaffected
  CHECK(fb.on_unit(make_nal(1, 0, 2), false).role == UnitRole::OrphanSlice);
  CHECK(fb.on_unit(make_nal(19, 0, 0), false).role == UnitRole::NextSlice);
}

// ---- ActiveSets (§7.3.8) ------------------------------------------------------------------------------------------

namespace {

struct Sets {
  h265::Sps sps = make_sps(256);
  h265::Pps pps;
  h265::Vps vps;
  std::vector<h265::SetDiagnostic> diags;

  Sets() { sps.log2_max_pic_order_cnt_lsb_minus4 = 4; }
};

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> b) {
  return b;
}

bool has_diag(const std::vector<h265::SetDiagnostic>& d, std::string_view code, Severity sev) {
  return std::ranges::any_of(d, [&](const h265::SetDiagnostic& x) { return x.code == code && x.severity == sev; });
}

}  // namespace

TEST_CASE("h265_active_sets: activation resolves PPS, SPS and VPS and builds the tile tables", "[index][h265][fast]") {
  Sets s;
  h265::ActiveSets as;
  std::vector<h265::SetDiagnostic>& d = s.diags;
  s.vps.vps_video_parameter_set_id = 0;
  s.sps.sps_video_parameter_set_id = 0;
  as.put_vps(s.vps, bytes({1}), 10, d);
  as.put_sps(s.sps, bytes({2}), 11, d);
  as.put_pps(s.pps, bytes({3}), 12, d);
  CHECK(d.empty());
  const auto act = as.activate(0, true, d);
  REQUIRE(act.has_value());
  CHECK(d.empty());
  CHECK(act->pps_unit == 12);
  CHECK(act->sps_unit == 11);
  CHECK(act->vps_unit == 10);
  CHECK(act->active->colBd.size() == 2);
  CHECK(act->active->pps == act->pps.get());
  CHECK(act->active->sps == act->sps.get());
  CHECK(as.pps_table().entries[0] == act->active.get());
  CHECK(as.pps_table().entries[1] == nullptr);
  CHECK(as.picture_open());
  as.end_picture();
  CHECK_FALSE(as.picture_open());
}

TEST_CASE("h265_active_sets: a PPS received before its SPS is activated once the SPS arrives", "[index][h265][fast]") {
  Sets s;
  h265::ActiveSets as;
  as.put_pps(s.pps, bytes({3}), 1, s.diags);
  CHECK(as.pps_table().entries[0] == nullptr);
  CHECK_FALSE(as.activate(0, true, s.diags).has_value());
  CHECK(has_diag(s.diags, "SYN-GEN-0102", Severity::Warning));
  as.put_sps(s.sps, bytes({2}), 2, s.diags);
  CHECK(as.pps_table().entries[0] != nullptr);
  const auto act = as.activate(0, true, s.diags);
  REQUIRE(act.has_value());
  CHECK(has_diag(s.diags, "SYN-H265-0022", Severity::Warning));  // no VPS in the stream
  const size_t warnings = s.diags.size();
  as.end_picture();
  REQUIRE(as.activate(0, true, s.diags).has_value());
  CHECK(s.diags.size() == warnings);  // the missing VPS is reported once per SPS generation
  CHECK_FALSE(act->vps);
}

TEST_CASE("h265_active_sets: missing PPS", "[index][h265][fast]") {
  h265::ActiveSets as;
  std::vector<h265::SetDiagnostic> d;
  CHECK_FALSE(as.activate(5, true, d).has_value());
  REQUIRE(d.size() == 1);
  CHECK(d[0].code == "SYN-H265-0011");
  CHECK(d[0].severity == Severity::Error);
  CHECK(d[0].message == "slice references missing PPS 5");
  CHECK_FALSE(as.activate(200, true, d).has_value());
}

TEST_CASE("h265_active_sets: replace before a picture, identical within it", "[index][h265][fast]") {
  Sets s;
  h265::ActiveSets as;
  as.put_sps(s.sps, bytes({2}), 1, s.diags);
  as.put_pps(s.pps, bytes({3}), 2, s.diags);
  const uint64_t gen1 = as.pps_generation(0);
  REQUIRE(as.activate(0, true, s.diags).has_value());
  s.diags.clear();
  // a repeat of the active PPS inside the picture changes nothing
  as.put_pps(s.pps, bytes({3}), 3, s.diags);
  CHECK(s.diags.empty());
  CHECK(as.pps_generation(0) == gen1);
  // different content inside the picture: Error, the new PPS is held back
  h265::Pps changed = s.pps;
  changed.init_qp_minus26 = 7;
  as.put_pps(changed, bytes({4}), 4, s.diags);
  REQUIRE(s.diags.size() == 1);
  CHECK(s.diags[0].code == "SYN-H265-0021");
  CHECK(s.diags[0].severity == Severity::Error);
  CHECK(s.diags[0].message == "PPS 0 changed within a picture");
  CHECK(as.pps(0)->init_qp_minus26 == 0);
  // the held-back copy replaces the entry at the next picture
  as.end_picture();
  CHECK(as.pps(0)->init_qp_minus26 == 7);
  CHECK(as.pps_generation(0) != gen1);
  CHECK(as.pps_table().entries[0]->pps->init_qp_minus26 == 7);
  // a PPS that arrives while no picture is open (or for another id) simply replaces the entry
  s.diags.clear();
  h265::Pps other = s.pps;
  other.pps_pic_parameter_set_id = 3;
  REQUIRE(as.activate(0, true, s.diags).has_value());
  as.put_pps(other, bytes({5}), 5, s.diags);
  CHECK(s.diags.empty());
  CHECK(as.pps(3) != nullptr);
}

TEST_CASE("h265_active_sets: an activation keeps its parameter sets alive after they are replaced",
          "[index][h265][fast]") {
  Sets s;
  h265::ActiveSets as;
  as.put_sps(s.sps, bytes({2}), 1, s.diags);
  as.put_pps(s.pps, bytes({3}), 2, s.diags);
  const auto act = as.activate(0, true, s.diags);
  REQUIRE(act.has_value());
  as.end_picture();
  h265::Sps newer = s.sps;
  newer.MaxPicOrderCntLsb = 32;
  as.put_sps(newer, bytes({9}), 7, s.diags);
  CHECK(act->sps->MaxPicOrderCntLsb == 256);
  CHECK(act->active->sps->MaxPicOrderCntLsb == 256);
  CHECK(as.sps(0)->MaxPicOrderCntLsb == 32);
  CHECK(as.pps_table().entries[0]->sps->MaxPicOrderCntLsb == 32);
}
