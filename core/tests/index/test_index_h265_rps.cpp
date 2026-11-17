// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the H.265 index models (chapter 7 §7.3.8–§7.3.10.2, §7.4.4, WI-1.115): parameter-set activation,
// frame-boundary detection, POC with prevTid0Pic, RPS derivation, reference marking and RefPicList0/1.
//
// Oracles. hm_poc_expected.h holds the POC and RefPicList0/1 POCs of the first slice of every picture, recorded from
// HM-18.0 (tests/golden/h265/<id>.hm-poc.json.gz). The always-run cases use short sequences of those streams
// (POC_A_Bossen_3, RPS_A_docomo_5, NUT_A_ericsson_5: their LSBs and RPS are in `trace_headers`).
// The [needs-corpus] cases run the whole chain (unit scan, parsers, ActiveSets, FrameBoundary, PocState, Dpb)
// over the conformance streams and skip when the corpus is absent.
//
// WI-1.123 runs the same models through the StreamIndex builder: FrameInfo.declared_refs / dpb_after, the RASL
// skip bitmap of the RAP map, the conformance window and the picture after an end of sequence. The always-run cases
// use the x265 open-GOP seed of the index_builder fuzz target, as written and edited (EOS inserted, CRA turned into
// a BLA).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hm_poc_expected.h"
#include "index/h265_index.h"
#include "index/rap_map.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/active_sets.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

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

struct Delta {
  int32_t delta;
  bool used;
};

// A slice header with its own st_ref_pic_set. `neg` are the S0 deltas (negative, closest first), `pos` the S1 deltas.
h265::SliceSegmentHeader make_sh(uint32_t lsb, uint32_t slice_type, std::vector<Delta> neg = {},
                                 std::vector<Delta> pos = {}, uint32_t l0_minus1 = 0, uint32_t l1_minus1 = 0) {
  h265::SliceSegmentHeader sh;
  sh.first_slice_segment_in_pic_flag = true;
  sh.slice_type = slice_type;
  sh.slice_pic_order_cnt_lsb = lsb;
  sh.short_term_ref_pic_set_sps_flag = false;
  h265::StRefPicSet& s = sh.st_ref_pic_set;
  s.NumNegativePics = static_cast<uint32_t>(neg.size());
  s.NumPositivePics = static_cast<uint32_t>(pos.size());
  s.NumDeltaPocs = s.NumNegativePics + s.NumPositivePics;
  uint32_t used = 0;
  for (size_t i = 0; i < neg.size(); ++i) {
    s.DeltaPocS0[i] = neg[i].delta;
    s.UsedByCurrPicS0[i] = neg[i].used;
    used += neg[i].used ? 1U : 0U;
  }
  for (size_t i = 0; i < pos.size(); ++i) {
    s.DeltaPocS1[i] = pos[i].delta;
    s.UsedByCurrPicS1[i] = pos[i].used;
    used += pos[i].used ? 1U : 0U;
  }
  sh.NumPicTotalCurr = used;
  sh.num_ref_idx_l0_active_minus1 = l0_minus1;
  sh.num_ref_idx_l1_active_minus1 = l1_minus1;
  return sh;
}

void add_long_term(h265::SliceSegmentHeader& sh, uint32_t poc_lsb, bool used, std::optional<uint64_t> msb_cycle = {}) {
  const uint32_t i = sh.num_long_term_pics++;
  sh.PocLsbLt[i] = poc_lsb;
  sh.poc_lsb_lt[i] = poc_lsb;
  sh.UsedByCurrPicLt[i] = used;
  sh.used_by_curr_pic_lt_flag[i] = used;
  sh.delta_poc_msb_present_flag[i] = msb_cycle.has_value();
  sh.DeltaPocMsbCycleLt[i] = msb_cycle.value_or(0);
  sh.NumPicTotalCurr += used ? 1U : 0U;
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

bool has_code(const std::vector<idx::IndexDiagnostic>& d, std::string_view code, Severity sev) {
  return std::ranges::any_of(d, [&](const idx::IndexDiagnostic& x) { return x.code == code && x.severity == sev; });
}

using Ints = std::vector<int32_t>;

// One picture through PocState and Dpb.
struct Walker {
  h265::Sps sps = make_sps(256);
  h265::Pps pps;
  idx::PocState poc;
  idx::Dpb dpb;
  int32_t next_index = 0;

  struct Out {
    idx::PocResult pr;
    idx::DpbStepResult d;
    int32_t decode_index = 0;
  };

  Out step(unsigned nut, unsigned tid, const h265::SliceSegmentHeader& sh) {
    const h265::NalHeader nal = make_nal(nut, tid);
    Out o;
    o.decode_index = next_index++;
    o.pr = poc.step(nal, sh, sps);
    o.d = dpb.step(o.decode_index, nal, sh, sps, pps, o.pr);
    return o;
  }
};

constexpr uint32_t kI = h265::kSliceI;
constexpr uint32_t kP = h265::kSliceP;
constexpr uint32_t kB = h265::kSliceB;

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
    const Ints expect = {0, 8, 4, 2, 1, 3};
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

// ---- RPS, marking, RefPicList (§7.4.4) ----------------------------------------------------------------------------

TEST_CASE("h265_refpiclist_unit: RPS_A_docomo_5 first pictures", "[index][h265][fast]") {
  // RPS_A_docomo_5 (trace_headers): B slices, PPS default num_ref_idx 2/2, picture 1 overrides to 1/1. The RPS are
  // SPS sets 8, 9, 10, 3, 4 (the h265_slice_rps expectations). Decode indices 0..5 have POC 0 8 4 2 1 3.
  Walker w;
  auto idr = w.step(19, 0, h265::SliceSegmentHeader{});
  CHECK(idr.pr.poc == 0);
  CHECK(idr.d.dpb_after == Ints{0});
  CHECK(idr.d.declared_refs.empty());

  struct Pic {
    uint32_t lsb;
    std::vector<Delta> neg, pos;
    uint32_t l0, l1;
    Ints list0, list1, declared, dpb_after;
  };
  const std::vector<Pic> pics = {
      {8, {{-8, true}}, {}, 0, 0, {0}, {0}, {0}, {0, 1}},
      {4, {{-4, true}}, {{4, true}}, 1, 1, {0, 1}, {1, 0}, {0, 1}, {0, 1, 2}},
      {2, {{-2, true}}, {{2, true}, {6, true}}, 1, 1, {0, 2}, {2, 1}, {0, 2, 1}, {0, 1, 2, 3}},
      {1, {{-1, true}}, {{1, true}, {3, true}, {7, true}}, 1, 1, {0, 3}, {3, 2}, {0, 3, 2}, {0, 1, 2, 3, 4}},
      {3, {{-1, true}, {-3, true}}, {{1, true}, {5, true}}, 1, 1, {3, 0}, {2, 1}, {3, 0, 2, 1}, {0, 1, 2, 3, 5}},
  };
  const Ints expect_poc = {8, 4, 2, 1, 3};
  for (size_t i = 0; i < pics.size(); ++i) {
    CAPTURE(i + 1);
    const Pic& p = pics[i];
    const auto o = w.step(i < 3 ? 1 : 0, 0, make_sh(p.lsb, kB, p.neg, p.pos, p.l0, p.l1));
    CHECK(o.pr.poc == expect_poc[i]);
    CHECK(o.d.ref_pic_list[0] == p.list0);
    CHECK(o.d.ref_pic_list[1] == p.list1);
    CHECK(o.d.declared_refs == p.declared);
    CHECK(o.d.dpb_after == p.dpb_after);
    CHECK(o.d.diagnostics.empty());
  }
}

TEST_CASE("h265_rps: marking drops pictures that no RPS list names; Foll pictures stay without being declared",
          "[index][h265][fast]") {
  Walker w;
  (void)w.step(19, 0, h265::SliceSegmentHeader{});                          // di 0, POC 0
  (void)w.step(1, 0, make_sh(1, kP, {{-1, true}}));                         // di 1, POC 1
  (void)w.step(1, 0, make_sh(2, kP, {{-1, true}, {-2, false}}));            // di 2, POC 2: refs 1, keeps 0 as Foll
  const auto o3 = w.step(1, 0, make_sh(3, kP, {{-1, true}, {-3, false}}));  // di 3, POC 3: refs 2, Foll POC 0
  CHECK(o3.d.declared_refs == Ints{2});
  CHECK(o3.d.dpb_after == Ints{0, 2, 3});  // POC 1 dropped (named by no list), POC 0 kept via StFoll
  CHECK(o3.d.sets.st_foll.size() == 1);
  CHECK(o3.d.sets.st_foll[0].decode_index == 0);
  // POC 1 is gone: naming it again is a missing reference
  const auto o4 = w.step(1, 0, make_sh(4, kP, {{-3, true}}));
  CHECK(o4.d.ref_pic_list[0] == Ints{idx::kNoRef});
}

TEST_CASE("h265_rps: missing references", "[index][h265][fast]") {
  SECTION("StCurr miss is Error SYN-H265-0102, the list keeps a no-reference entry, declared_refs drops it") {
    Walker w;
    (void)w.step(19, 0, h265::SliceSegmentHeader{});
    const auto o = w.step(1, 0, make_sh(4, kP, {{-4, true}, {-2, true}}, {}, 1));
    CHECK(has_code(o.d.diagnostics, "SYN-H265-0102", Severity::Error));
    CHECK(o.d.ref_pic_list[0] == Ints{0, idx::kNoRef});  // S0 is {-4, -2}: POC 0 exists, POC 2 does not
    CHECK(o.d.declared_refs == Ints{0});
  }
  SECTION("StFoll miss is not a diagnostic") {
    Walker w;
    (void)w.step(19, 0, h265::SliceSegmentHeader{});
    const auto o = w.step(1, 0, make_sh(2, kP, {{-2, true}, {-1, false}}));
    CHECK(o.d.diagnostics.empty());
  }
  SECTION("RASL picture after a CRA start: Info instead of Error") {
    Walker w;
    (void)w.step(21, 0, h265::SliceSegmentHeader{});  // CRA, first picture, POC 0
    h265::SliceSegmentHeader rasl =
        make_sh(250, kB, {{-2, true}}, {{2, true}}, 1, 1);  // POC -6, refs -8 and -4: absent
    const auto o = w.step(8, 0, rasl);
    CHECK(o.pr.rasl_skipped);
    CHECK(has_code(o.d.diagnostics, "SYN-H265-0102", Severity::Info));
    CHECK_FALSE(has_code(o.d.diagnostics, "SYN-H265-0102", Severity::Error));
  }
  SECTION("a CRA with NoRaslOutputFlag 1 clears the DPB and names no missing pictures") {
    Walker w;
    (void)w.step(19, 0, h265::SliceSegmentHeader{});
    (void)w.step(1, 0, make_sh(8, kP, {{-8, true}}));
    const auto cra = w.step(21, 0, make_sh(16, kI, {{-8, true}}));  // CRA mid-stream keeps its RPS
    CHECK_FALSE(cra.pr.no_rasl_output_flag);
    CHECK(cra.d.dpb_after == Ints{1, 2});
    w.poc.on_eos();
    const auto cra2 = w.step(21, 0, make_sh(0, kI, {{-4, true}}));
    CHECK(cra2.pr.no_rasl_output_flag);
    CHECK(cra2.d.diagnostics.empty());
    CHECK(cra2.d.dpb_after == Ints{3});
  }
}

TEST_CASE("h265_rps: long-term entries match by LSB unless delta_poc_msb_present_flag", "[index][h265][fast]") {
  Walker w;
  w.sps = make_sps(16);
  (void)w.step(19, 0, h265::SliceSegmentHeader{});                 // di 0, POC 0
  (void)w.step(1, 0, make_sh(5, kP, {{-5, true}}));                // di 1, POC 5
  (void)w.step(1, 0, make_sh(9, kP, {{-4, true}, {-9, false}}));   // di 2, POC 9
  (void)w.step(1, 0, make_sh(13, kP, {{-4, true}, {-8, false}}));  // di 3, POC 13; DPB: POC 5(?), 9, 13, 0 kept
  // POC 18 (lsb 2, MSB 16): long-term LSB 13 (no MSB info) matches POC 13; LSB 5 with MSB cycle 1 matches POC 5 only
  // when the full POC agrees: 5 + 18 - 1*16 - (18 & 15) = 5.
  h265::SliceSegmentHeader sh = make_sh(2, kP, {{-9, true}}, {}, 2);
  add_long_term(sh, 13, true);
  add_long_term(sh, 5, true, 1);
  const auto o = w.step(1, 0, sh);
  CHECK(o.pr.poc == 18);
  REQUIRE(o.d.sets.lt_curr.size() == 2);
  CHECK(o.d.sets.lt_curr[0].poc == 13);
  CHECK(o.d.sets.lt_curr[0].decode_index == 3);
  CHECK(o.d.sets.lt_curr[1].poc == 5);
  CHECK(o.d.sets.lt_curr[1].decode_index == 1);
  CHECK(o.d.sets.st_curr_before.size() == 1);
  CHECK(o.d.sets.st_curr_before[0].decode_index == 2);
  // list: StCurrBefore (POC 9), StCurrAfter (none), LtCurr (13, 5), repeated to three entries
  CHECK(o.d.ref_pic_list[0] == Ints{2, 3, 1});
  CHECK(o.d.declared_refs == Ints{2, 3, 1});
  CHECK(o.d.dpb_after == Ints{1, 2, 3, 4});
  // an LSB-only entry matches any reference picture with those LSBs, short or long
  h265::SliceSegmentHeader sh3 = make_sh(4, kP);
  add_long_term(sh3, 13, true);
  const auto o3 = w.step(1, 0, sh3);  // POC 20
  CHECK(o3.d.sets.lt_curr[0].decode_index == 3);
  // the LT picture is now long-term: a short-term entry with the same POC does not see it
  const auto o2 = w.step(1, 0, make_sh(5, kP, {{-8, true}}));  // POC 21 wants short-term POC 13
  CHECK(has_code(o2.d.diagnostics, "SYN-H265-0102", Severity::Error));
  // a miss in LtCurr is an Error
  h265::SliceSegmentHeader sh4 = make_sh(5, kP);
  add_long_term(sh4, 7, true);
  CHECK(has_code(w.step(1, 0, sh4).d.diagnostics, "SYN-H265-0102", Severity::Error));
}

TEST_CASE("h265_rps: an LSB-only long-term entry lists the full POC of the matched picture", "[index][h265][fast]") {
  Walker w;
  w.sps = make_sps(16);
  (void)w.step(19, 0, h265::SliceSegmentHeader{});               // di 0, POC 0
  (void)w.step(1, 0, make_sh(8, kP, {{-8, true}}));              // di 1, POC 8
  (void)w.step(1, 0, make_sh(13, kP, {{-5, true}}));             // di 2, POC 13
  (void)w.step(1, 0, make_sh(2, kP, {{-5, true}, {-3, true}}));  // di 3, POC 18
  h265::SliceSegmentHeader sh = make_sh(6, kP, {{-4, true}});    // POC 22 (lsb 6, msb 16)
  add_long_term(sh, 13, true);                                   // LSB 13 matches POC 13
  const auto o = w.step(1, 0, sh);
  REQUIRE(o.d.sets.lt_curr.size() == 1);
  CHECK(o.d.sets.lt_curr[0].decode_index == 2);
  CHECK(o.d.sets.lt_curr[0].poc == 13);
  // a picture with the same LSBs in a later MSB cycle: POC 29 = lsb 13 + 16
  h265::SliceSegmentHeader sh2 = make_sh(13, kP, {{-7, true}});  // POC 29, refs POC 22
  const auto o2 = w.step(1, 0, sh2);
  CHECK(o2.pr.poc == 29);
  h265::SliceSegmentHeader sh3 = make_sh(1, kP, {{-4, true}});  // POC 33
  add_long_term(sh3, 13, true);
  const auto o3 = w.step(1, 0, sh3);
  REQUIRE(o3.d.sets.lt_curr.size() == 1);
  CHECK(o3.d.sets.lt_curr[0].poc == 29);        // not 13
  CHECK(o3.d.ref_pic_list_poc[0] == Ints{29});  // RefPicList0 has one active entry: StCurrBefore POC 29 comes first
}

TEST_CASE("h265_reflist: modification, repetition up to NumRpsCurrTempList and RefPicList1 order",
          "[index][h265][fast]") {
  Walker w;
  (void)w.step(19, 0, h265::SliceSegmentHeader{});                      // di 0 POC 0
  (void)w.step(1, 0, make_sh(8, kP, {{-8, true}}));                     // di 1 POC 8
  (void)w.step(1, 0, make_sh(4, kB, {{-4, true}}, {{4, true}}, 1, 1));  // di 2 POC 4: before {0}, after {8}
  // POC 2 sees 0 before; 4 and 8 after. Four entries requested from three pictures: the temp list repeats.
  h265::SliceSegmentHeader sh = make_sh(2, kB, {{-2, true}}, {{2, true}, {6, true}}, 3, 3);
  const auto plain = w.step(1, 0, sh);
  CHECK(plain.d.ref_pic_list[0] == Ints{0, 2, 1, 0});  // before, after, repeat from the start
  CHECK(plain.d.ref_pic_list[1] == Ints{2, 1, 0, 2});  // after first, then before
  // ref_pic_list_modification: entries index RefPicListTemp
  h265::SliceSegmentHeader mod = make_sh(1, kB, {{-1, true}}, {{1, true}, {3, true}}, 1, 1);
  mod.ref_pic_lists_modification.emplace();
  mod.ref_pic_lists_modification->ref_pic_list_modification_flag_l0 = true;
  mod.ref_pic_lists_modification->list_entry_l0 = {2, 0};
  mod.ref_pic_lists_modification->ref_pic_list_modification_flag_l1 = false;
  const auto m = w.step(0, 0, mod);          // di 4 POC 1: refs POC 0 (di 0), POC 2 (di 3), POC 4 (di 2)
  CHECK(m.d.ref_pic_list[0] == Ints{2, 0});  // Temp0 = {0, 3, 2}: entries 2 and 0
  CHECK(m.d.ref_pic_list[1] == Ints{3, 2});  // unmodified: after {3, 2}, then before
}

TEST_CASE("h265_reflist: pps_curr_pic_ref_enabled_flag puts the current picture in the lists", "[index][h265][fast]") {
  Walker w;
  w.pps.scc_extension.pps_curr_pic_ref_enabled_flag = true;
  (void)w.step(19, 0, h265::SliceSegmentHeader{});  // di 0
  h265::SliceSegmentHeader sh = make_sh(1, kP, {{-1, true}}, {}, 1);
  sh.NumPicTotalCurr = 2;  // one RPS picture + the current picture (7-55)
  const auto o = w.step(1, 0, sh);
  CHECK(o.d.ref_pic_list[0] == Ints{0, 1});  // temp {0, cur}; the current picture is di 1
  // two RPS entries into a 2-entry list: the current picture replaces the last entry only when the temp list is longer
  h265::SliceSegmentHeader sh2 = make_sh(2, kP, {{-1, true}, {-2, true}}, {}, 1);
  sh2.NumPicTotalCurr = 3;
  const auto o2 = w.step(1, 0, sh2);
  CHECK(o2.d.ref_pic_list[0] == Ints{1, 2});  // temp {1, 0, cur}; entry 1 replaced by cur (di 2)
  // without the flag the current picture never appears
  Walker w2;
  (void)w2.step(19, 0, h265::SliceSegmentHeader{});
  const auto o3 = w2.step(1, 0, make_sh(1, kP, {{-1, true}}, {}, 1));
  CHECK(o3.d.ref_pic_list[0] == Ints{0, 0});
}

TEST_CASE("h265_dpb: IDR and I slices produce no lists", "[index][h265][fast]") {
  Walker w;
  (void)w.step(19, 0, h265::SliceSegmentHeader{});
  const auto o = w.step(1, 0, make_sh(1, kI, {{-1, true}}));
  CHECK(o.d.ref_pic_list[0].empty());
  CHECK(o.d.declared_refs.empty());
  CHECK(o.d.dpb_after == Ints{0, 1});
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

// ---- conformance streams: the whole chain (needs the corpus) ------------------------------------------------------

namespace {

// Units -> parsers -> ActiveSets -> FrameBoundary -> PocState -> Dpb, as WI-1.121 will wire them.
class Chain {
 public:
  struct Picture {
    h265::NalHeader nal;
    h265::SliceSegmentHeader sh;
    idx::PocResult pr;
    idx::DpbStepResult d;
    int32_t decode_index = 0;
  };

  void add(std::span<const uint8_t> nal_bytes) {
    const std::vector<uint8_t> nal(nal_bytes.begin(), nal_bytes.end());
    const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
    ovt::BitReader br(rbsp.rbsp);
    ovt::syntax::SyntaxBuilder b(br);
    const h265::NalHeader h = h265::parse_nal_header(br, b);
    const uint64_t unit = unit_++;
    if (h.nuh_layer_id != 0) {
      return;
    }
    if (!h.is_vcl) {
      const idx::BoundaryDecision bd = fb_.on_unit(h, false);
      if (bd.starts_access_unit) {
        sets_.end_picture();
      }
      if (bd.is_eos) {
        poc_.on_eos();
      }
      if (h.nal_unit_type == h265::kVpsNut) {
        sets_.put_vps(h265::parse_vps(br, b), rbsp.rbsp, unit, set_diags);
      } else if (h.nal_unit_type == h265::kSpsNut) {
        sets_.put_sps(h265::parse_sps(br, b, h.nuh_layer_id), rbsp.rbsp, unit, set_diags);
      } else if (h.nal_unit_type == h265::kPpsNut) {
        sets_.put_pps(h265::parse_pps(br, b), rbsp.rbsp, unit, set_diags);
      }
      return;
    }
    Picture p;
    p.nal = h;
    p.sh = h265::parse_slice_segment_header(br, b, h, sets_.pps_table(),
                                            prev_independent_ ? &*prev_independent_ : nullptr);
    if (!p.sh.dependent_slice_segment_flag) {
      prev_independent_ = p.sh;
    }
    const idx::BoundaryDecision bd = fb_.on_unit(h, p.sh.first_slice_segment_in_pic_flag);
    if (bd.role != idx::UnitRole::FirstSlice) {
      return;
    }
    const auto act = sets_.activate(p.sh.slice_pic_parameter_set_id, true, set_diags);
    REQUIRE(act.has_value());
    p.decode_index = next_index_++;
    p.pr = poc_.step(h, p.sh, *act->sps);
    p.d = dpb_.step(p.decode_index, h, p.sh, *act->sps, *act->pps, p.pr);
    pictures.push_back(std::move(p));
  }

  void add_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::vector<uint8_t>> units;
    ovt::syntax::scan_annexb(std::span<const uint8_t>(data), 0, ovt::syntax::kNoContainerSample,
                             [&units](const ovt::syntax::ScannedUnit& u) {
                               if (u.bytes.size() >= 2) {
                                 units.emplace_back(u.bytes.begin(), u.bytes.end());
                               }
                             });
    for (const auto& u : units) {
      add(u);
    }
  }

  [[nodiscard]] Ints pocs(size_t n) const {
    Ints out;
    for (size_t i = 0; i < pictures.size() && i < n; ++i) {
      out.push_back(pictures[i].pr.poc);
    }
    return out;
  }

  [[nodiscard]] size_t error_count() const {
    size_t n = 0;
    for (const Picture& p : pictures) {
      for (const idx::IndexDiagnostic& d : p.d.diagnostics) {
        n += d.severity == Severity::Error ? 1U : 0U;
      }
    }
    return n;
  }

  std::vector<Picture> pictures;
  std::vector<h265::SetDiagnostic> set_diags;

 private:
  h265::ActiveSets sets_;
  idx::FrameBoundary fb_;
  idx::PocState poc_;
  idx::Dpb dpb_;
  std::optional<h265::SliceSegmentHeader> prev_independent_;
  uint64_t unit_ = 0;
  int32_t next_index_ = 0;
};

}  // namespace

namespace {

struct HmPicture {
  int32_t poc = 0;
  unsigned nut = 0;
  unsigned tid = 0;
  unsigned slice_type = 0;
  Ints l0, l1;
};

Ints parse_ints(std::string_view s) {
  Ints out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') {
      ++i;
    }
    size_t j = i;
    while (j < s.size() && s[j] != ' ') {
      ++j;
    }
    if (j > i) {
      out.push_back(std::stoi(std::string(s.substr(i, j - i))));
    }
    i = j;
  }
  return out;
}

// Parses the records of hm_poc_expected.h: "poc nut tid slice_type | l0 | l1;".
std::vector<HmPicture> parse_hm(std::string_view text) {
  std::vector<HmPicture> out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t end = text.find(';', pos);
    if (end == std::string_view::npos) {
      break;
    }
    const std::string_view rec = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t bar1 = rec.find('|');
    const size_t bar2 = rec.find('|', bar1 + 1);
    const Ints head = parse_ints(rec.substr(0, bar1));
    HmPicture p;
    p.poc = head[0];
    p.nut = static_cast<unsigned>(head[1]);
    p.tid = static_cast<unsigned>(head[2]);
    p.slice_type = static_cast<unsigned>(head[3]);
    p.l0 = parse_ints(rec.substr(bar1 + 1, bar2 - bar1 - 1));
    p.l1 = parse_ints(rec.substr(bar2 + 1));
    out.push_back(std::move(p));
  }
  return out;
}

// Indexes the whole stream and compares every picture with HM's trace of the first slice of each picture. HM does
// not decode the RASL pictures of a CRA/BLA at random access, so those are left out of OVT's side. A reference
// that HM replaced by a generated picture is "no reference picture" in OVT's list; its POC is still listed.
void check_against_hm(std::string_view id, std::string_view hm_text) {
  Chain c;
  c.add_file(ovt::test::corpus_or_skip(id));
  const std::vector<HmPicture> hm = parse_hm(hm_text);
  std::vector<const Chain::Picture*> ours;
  for (const Chain::Picture& p : c.pictures) {
    if (!p.pr.rasl_skipped) {
      ours.push_back(&p);
    }
  }
  REQUIRE(ours.size() == hm.size());
  for (size_t i = 0; i < hm.size(); ++i) {
    CAPTURE(id, i);
    CHECK(ours[i]->pr.poc == hm[i].poc);
    CHECK(ours[i]->nal.nal_unit_type == hm[i].nut);
    CHECK(ours[i]->nal.TemporalId == hm[i].tid);
    CHECK(ours[i]->sh.slice_type == hm[i].slice_type);
    CHECK(ours[i]->d.ref_pic_list_poc[0] == hm[i].l0);
    CHECK(ours[i]->d.ref_pic_list_poc[1] == hm[i].l1);
  }
  CHECK(c.error_count() == 0);
}

}  // namespace

// The same RPS_A_docomo_5 walk as h265_refpiclist_unit, from the RPS in `trace_headers`, against HM's trace (needs no
// corpus): the parser of the expected table and the first five lists.
TEST_CASE("h265_refpiclist_unit: RPS_A_docomo_5 first pictures equal the HM trace", "[index][h265][fast]") {
  const std::vector<HmPicture> hm = parse_hm(ovt::test::h265::kHm_RPS_A_docomo_5);
  REQUIRE(hm.size() == 44);
  Walker w;
  (void)w.step(19, 0, h265::SliceSegmentHeader{});
  const std::array<std::array<std::vector<Delta>, 2>, 5> rps = {{{{{{-8, true}}, {}}},
                                                                 {{{{-4, true}}, {{4, true}}}},
                                                                 {{{{-2, true}}, {{2, true}, {6, true}}}},
                                                                 {{{{-1, true}}, {{1, true}, {3, true}, {7, true}}}},
                                                                 {{{{-1, true}, {-3, true}}, {{1, true}, {5, true}}}}}};
  const std::array<uint32_t, 5> lsb = {8, 4, 2, 1, 3};
  for (size_t i = 0; i < 5; ++i) {
    CAPTURE(i + 1);
    const auto o = w.step(i < 3 ? 1 : 0, 0, make_sh(lsb[i], kB, rps[i][0], rps[i][1], i == 0 ? 0 : 1, i == 0 ? 0 : 1));
    CHECK(o.pr.poc == hm[i + 1].poc);
    CHECK(o.d.ref_pic_list_poc[0] == hm[i + 1].l0);
    CHECK(o.d.ref_pic_list_poc[1] == hm[i + 1].l1);
  }
}

// AT1 and AT2 (POC_A_Bossen_3, RPS_A_docomo_5) and the further RPS and random-access streams: POC and the
// RefPicList0/1 POCs of the first slice of every picture equal the sequences recorded from HM-18.0.
TEST_CASE("h265_refpiclist_unit: POC and RefPicLists equal the HM trace", "[index][h265][fast][needs-corpus]") {
  namespace e = ovt::test::h265;
  SECTION("POC_A_Bossen_3") {
    check_against_hm("jctvc_POC_A_Bossen_3", e::kHm_POC_A_Bossen_3);
  }
  SECTION("RPS_A_docomo_5") {
    check_against_hm("jctvc_RPS_A_docomo_5", e::kHm_RPS_A_docomo_5);
  }
  SECTION("RPS_B_qualcomm_5") {
    check_against_hm("jctvc_RPS_B_qualcomm_5", e::kHm_RPS_B_qualcomm_5);
  }
  SECTION("RPS_C_ericsson_5") {
    check_against_hm("jctvc_RPS_C_ericsson_5", e::kHm_RPS_C_ericsson_5);
  }
  SECTION("RPS_D_ericsson_6") {
    check_against_hm("jctvc_RPS_D_ericsson_6", e::kHm_RPS_D_ericsson_6);
  }
  SECTION("RPS_E_qualcomm_5") {
    check_against_hm("jctvc_RPS_E_qualcomm_5", e::kHm_RPS_E_qualcomm_5);
  }
  SECTION("RPS_F_docomo_2") {
    check_against_hm("jctvc_RPS_F_docomo_2", e::kHm_RPS_F_docomo_2);
  }
  SECTION("RAP_B_Bossen_2") {
    check_against_hm("jctvc_RAP_B_Bossen_2", e::kHm_RAP_B_Bossen_2);
  }
  // AT4: sub-layer non-reference pictures do not update prevTid0Pic
  SECTION("NUT_A_ericsson_5") {
    check_against_hm("jctvc_NUT_A_ericsson_5", e::kHm_NUT_A_ericsson_5);
  }
}

TEST_CASE("h265_rap_bitmap: RAP_B_Bossen_2 flags RASL pictures and the CRA", "[index][h265][fast][needs-corpus]") {
  Chain c;
  c.add_file(ovt::test::corpus_or_skip("jctvc_RAP_B_Bossen_2"));
  REQUIRE(c.pictures.size() >= 2);
  size_t cras = 0;
  size_t rasl = 0;
  for (const Chain::Picture& p : c.pictures) {
    if (p.nal.is_cra) {
      ++cras;
      CHECK(p.pr.is_rap);
    }
    if (p.nal.is_rasl) {
      ++rasl;
      CHECK(p.pr.is_rasl);
    }
  }
  CHECK(cras >= 1);
  CHECK(rasl >= 1);
  CHECK(c.error_count() == 0);
}

// ---- WI-1.123: the DPB model through the StreamIndex builder ------------------------------------------------------

namespace {

namespace fb = ovt::fb;
using Bytes = std::vector<uint8_t>;

Bytes read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The x265 open-GOP seed of the index_builder fuzz target without its selector byte. Decode order:
// IDR P B B | CRA RASL RASL P B B P B; the two RASL pictures reference the P picture before the CRA.
Bytes open_gop() {
  Bytes b = read_bytes(std::filesystem::path(OVT_INDEX_BUILDER_SEEDS) / "seed_3_h265_open_gop.bin");
  REQUIRE(b.size() > 1);
  b.erase(b.begin());
  return b;
}

// Byte offsets of the start codes (00 00 01) of an Annex B stream.
std::vector<size_t> start_codes(const Bytes& b) {
  std::vector<size_t> out;
  for (size_t i = 0; i + 2 < b.size(); ++i) {
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
      out.push_back(i);
    }
  }
  return out;
}

// Offset of the NAL header of the first unit of type `type` in `b`.
size_t first_unit_of_type(const Bytes& b, unsigned type) {
  for (const size_t sc : start_codes(b)) {
    if (sc + 3 < b.size() && ((b[sc + 3] >> 1U) & 0x3FU) == type) {
      return sc + 3;
    }
  }
  FAIL("no unit of type " << type);
  return 0;
}

// Indexes an Annex B H.265 stream with the builder and checks the V-031 rules on the result.
Bytes index_h265(std::span<const uint8_t> stream) {
  ovt::index::StreamIndexBuilder b(ovt::index::BuilderOptions{fb::Codec::H265});
  ovt::syntax::scan_annexb(stream, 0, ovt::syntax::kNoContainerSample, [&b](const ovt::syntax::ScannedUnit& u) {
    ovt::index::UnitInput in;
    in.file_offset = u.file_offset;
    in.bytes = u.bytes;
    in.prefix_span = u.prefix_span;
    in.sample_index = u.container_sample_index;
    b.add_unit(in);
  });
  const flatbuffers::DetachedBuffer out = b.finish(std::array<uint8_t, 32>{});
  Bytes index(out.data(), out.data() + out.size());
  for (const auto& v : ovt::model::validate_stream_index(index)) {
    FAIL_CHECK(v.rule << ": " << v.message << " at " << v.path);
  }
  return index;
}

const fb::StreamIndex& root_of(const Bytes& index) {
  return *flatbuffers::GetRoot<fb::StreamIndex>(index.data());
}

Ints to_ints(const flatbuffers::Vector<int32_t>* v) {
  return v == nullptr ? Ints{} : Ints(v->begin(), v->end());
}

// nal_unit_type of the first VCL unit of frame `f`.
unsigned vcl_type(const fb::StreamIndex& si, const fb::FrameInfo& f) {
  for (const uint32_t u : *f.units()) {
    if (si.units()->Get(u)->type() < 32) {
      return si.units()->Get(u)->type();
    }
  }
  return 64;
}

bool is_rasl(const fb::StreamIndex& si, const fb::FrameInfo& f) {
  const unsigned t = vcl_type(si, f);
  return t == 8 || t == 9;
}

// The POCs of the frames `refs` names.
Ints pocs_of(const fb::StreamIndex& si, const Ints& refs) {
  Ints out;
  for (const int32_t r : refs) {
    out.push_back(static_cast<int32_t>(si.frames()->Get(static_cast<uint32_t>(r))->poc()));
  }
  return out;
}

// Diagnostics of the index with `code` and `severity`.
size_t count_diag(const fb::StreamIndex& si, std::string_view code, fb::Severity severity) {
  size_t n = 0;
  if (si.diagnostics() != nullptr) {
    for (const auto* d : *si.diagnostics()) {
      n += d->code()->string_view() == code && d->severity() == severity ? 1U : 0U;
    }
  }
  return n;
}

// The builder's declared_refs / dpb_after equal those of the Chain over the same bytes (the current picture left
// out of declared_refs, #733).
void check_builder_matches_chain(const Bytes& stream) {
  const Bytes index = index_h265(stream);
  const fb::StreamIndex& si = root_of(index);
  Chain c;
  ovt::syntax::scan_annexb(std::span<const uint8_t>(stream), 0, ovt::syntax::kNoContainerSample,
                           [&c](const ovt::syntax::ScannedUnit& u) {
                             if (u.bytes.size() >= 2) {
                               c.add(u.bytes);
                             }
                           });
  REQUIRE(si.frames()->size() == c.pictures.size());
  for (const Chain::Picture& p : c.pictures) {
    CAPTURE(p.decode_index);
    const fb::FrameInfo& f = *si.frames()->Get(static_cast<uint32_t>(p.decode_index));
    Ints declared = p.d.declared_refs;
    std::erase(declared, p.decode_index);
    CHECK(to_ints(f.declared_refs()) == declared);
    CHECK(to_ints(f.dpb_after()) == p.d.dpb_after);
    CHECK(f.poc() == p.pr.poc);
  }
}

}  // namespace

TEST_CASE("h265_index_dpb: declared_refs and dpb_after of an open GOP through the builder", "[index][h265][fast]") {
  const Bytes stream = open_gop();
  const Bytes index = index_h265(stream);
  const fb::StreamIndex& si = root_of(index);
  REQUIRE(si.frames()->size() == 12);
  const fb::FrameInfo& idr = *si.frames()->Get(0);
  CHECK(to_ints(idr.declared_refs()).empty());
  CHECK(to_ints(idr.dpb_after()) == Ints{0});
  for (uint32_t d = 1; d < 12; ++d) {
    CAPTURE(d);
    const fb::FrameInfo& f = *si.frames()->Get(d);
    const Ints dpb = to_ints(f.dpb_after());
    // Every picture is a reference picture once decoded (§7.4.4), and refers only to earlier pictures it keeps.
    CHECK(std::ranges::find(dpb, static_cast<int32_t>(d)) != dpb.end());
    CHECK(std::ranges::is_sorted(dpb));
    for (const int32_t r : to_ints(f.declared_refs())) {
      CHECK(r < static_cast<int32_t>(d));
      const Ints before = to_ints(si.frames()->Get(d - 1)->dpb_after());
      CHECK(std::ranges::find(before, r) != before.end());
    }
    if (f.type() != fb::FrameType::I) {
      CHECK_FALSE(to_ints(f.declared_refs()).empty());
    }
  }
  // A mid-stream CRA keeps the pictures before it: its RASL pictures reference the P picture before it.
  const fb::FrameInfo& cra = *si.frames()->Get(4);
  REQUIRE(vcl_type(si, cra) == 21);
  CHECK(to_ints(cra.dpb_after()).size() > 1);
  for (uint32_t d = 5; d <= 6; ++d) {
    CAPTURE(d);
    const fb::FrameInfo& rasl = *si.frames()->Get(d);
    REQUIRE(is_rasl(si, rasl));
    CHECK(rasl.shown());
    const Ints refs = to_ints(rasl.declared_refs());
    CHECK(std::ranges::any_of(refs, [](int32_t r) { return r < 4; }));
  }
  CHECK(count_diag(si, "SYN-H265-0102", fb::Severity::Error) == 0);
  check_builder_matches_chain(stream);
}

// AT3 without the corpus (#734): an end of sequence NAL before the CRA gives it NoRaslOutputFlag = 1, so its POC MSB
// is 0, the pictures before it leave the DPB and its RASL pictures are not output; their missing references are
// Info, not Error. A BLA_W_LP in place of the CRA does the same, and the RAP map skips its RASL pictures too.
TEST_CASE("h265_index_dpb: the picture after an end of sequence, and a BLA", "[index][h265][fast]") {
  const Bytes plain = open_gop();
  const size_t cra_at = first_unit_of_type(plain, 21);
  Bytes eos = plain;
  // 00 00 01 | nal_unit_type 36 (EOS_NUT), layer 0, TemporalId 0, inserted before the CRA's start code.
  const std::array<uint8_t, 5> eos_nal = {0x00, 0x00, 0x01, 0x48, 0x01};
  eos.insert(eos.begin() + static_cast<std::ptrdiff_t>(cra_at - 3), eos_nal.begin(), eos_nal.end());
  Bytes bla = plain;
  bla[cra_at] = static_cast<uint8_t>((16U << 1U) | (bla[cra_at] & 0x81U));  // BLA_W_LP

  for (const auto& [name, stream] : {std::pair{"eos", &eos}, std::pair{"bla", &bla}}) {
    SECTION(name) {
      const Bytes index = index_h265(*stream);
      const fb::StreamIndex& si = root_of(index);
      REQUIRE(si.frames()->size() == 12);
      const fb::FrameInfo& rap = *si.frames()->Get(4);
      CHECK(rap.is_rap());
      // POC MSB 0: the POC is the slice_pic_order_cnt_lsb of the plain stream's CRA, which follows POCs 0..3.
      const Bytes plain_index = index_h265(plain);
      const int64_t cra_poc = root_of(plain_index).frames()->Get(4)->poc();
      CHECK(rap.poc() == cra_poc);
      CHECK(to_ints(rap.dpb_after()) == Ints{4});
      CHECK(to_ints(rap.declared_refs()).empty());
      const ovt::index::RapMap raps(si);
      for (uint32_t d = 5; d <= 6; ++d) {
        CAPTURE(d);
        const fb::FrameInfo& rasl = *si.frames()->Get(d);
        CHECK_FALSE(rasl.shown());
        CHECK(std::ranges::none_of(to_ints(rasl.declared_refs()), [](int32_t r) { return r < 4; }));
        CHECK(raps.skip_when_starting_at_rap(d));
      }
      CHECK_FALSE(raps.skip_when_starting_at_rap(7));
      CHECK(count_diag(si, "SYN-H265-0102", fb::Severity::Error) == 0);
      CHECK(count_diag(si, "SYN-H265-0102", fb::Severity::Info) > 0);
      check_builder_matches_chain(*stream);
    }
  }
}

namespace {

// AT1: indexes the whole stream with the builder and compares every picture HM decoded with HM's record. HM does
// not decode the RASL pictures of a CRA with NoRaslOutputFlag = 1; in the index those are the RASL frames that are
// not shown. declared_refs resolved to POCs equals RefPicList0 ∪ RefPicList1 of HM (unique, L0 first).
void check_index_against_hm(std::string_view id, std::string_view hm_text) {
  const Bytes stream = read_bytes(ovt::test::corpus_or_skip(id));
  const Bytes index = index_h265(stream);
  const fb::StreamIndex& si = root_of(index);
  const std::vector<HmPicture> hm = parse_hm(hm_text);
  std::vector<const fb::FrameInfo*> ours;
  for (const fb::FrameInfo* f : *si.frames()) {
    if (!is_rasl(si, *f) || f->shown()) {
      ours.push_back(f);
    }
  }
  REQUIRE(ours.size() == hm.size());
  for (size_t i = 0; i < hm.size(); ++i) {
    CAPTURE(id, i);
    const fb::FrameInfo& f = *ours[i];
    CHECK(f.poc() == hm[i].poc);
    CHECK(vcl_type(si, f) == hm[i].nut);
    CHECK(f.temporal_id() == hm[i].tid);
    Ints expected;
    for (const Ints* list : {&hm[i].l0, &hm[i].l1}) {
      for (const int32_t poc : *list) {
        if (std::ranges::find(expected, poc) == expected.end()) {
          expected.push_back(poc);
        }
      }
    }
    CHECK(pocs_of(si, to_ints(f.declared_refs())) == expected);
  }
  CHECK(count_diag(si, "SYN-H265-0102", fb::Severity::Error) == 0);
}

}  // namespace

// WI-1.123 AT1: h265_refpiclist_vs_hm through the full builder, on the HEVC_v1 streams with an HM record.
TEST_CASE("h265_refpiclist_vs_hm: the builder's declared_refs equal HM's RefPicLists",
          "[index][h265][fast][needs-corpus]") {
  namespace e = ovt::test::h265;
  SECTION("POC_A_Bossen_3") {
    check_index_against_hm("jctvc_POC_A_Bossen_3", e::kHm_POC_A_Bossen_3);
  }
  SECTION("RPS_A_docomo_5") {
    check_index_against_hm("jctvc_RPS_A_docomo_5", e::kHm_RPS_A_docomo_5);
  }
  SECTION("RPS_B_qualcomm_5") {
    check_index_against_hm("jctvc_RPS_B_qualcomm_5", e::kHm_RPS_B_qualcomm_5);
  }
  SECTION("RPS_C_ericsson_5") {
    check_index_against_hm("jctvc_RPS_C_ericsson_5", e::kHm_RPS_C_ericsson_5);
  }
  SECTION("RPS_D_ericsson_6") {
    check_index_against_hm("jctvc_RPS_D_ericsson_6", e::kHm_RPS_D_ericsson_6);
  }
  SECTION("RPS_E_qualcomm_5") {
    check_index_against_hm("jctvc_RPS_E_qualcomm_5", e::kHm_RPS_E_qualcomm_5);
  }
  SECTION("RPS_F_docomo_2") {
    check_index_against_hm("jctvc_RPS_F_docomo_2", e::kHm_RPS_F_docomo_2);
  }
  SECTION("RAP_B_Bossen_2") {
    check_index_against_hm("jctvc_RAP_B_Bossen_2", e::kHm_RAP_B_Bossen_2);
  }
  SECTION("NUT_A_ericsson_5") {
    check_index_against_hm("jctvc_NUT_A_ericsson_5", e::kHm_NUT_A_ericsson_5);
  }
}

// WI-1.123 AT2: CONFWIN_A_Sony_1 (416x240 4:2:0, conf_win offsets 1/1/1/1 in chroma units, from its trace_headers
// golden) has crop {2, 2, 412, 236} in luma samples on every frame.
TEST_CASE("h265_index_crop: CONFWIN_A_Sony_1 crop is the conformance window in luma samples",
          "[index][h265][fast][needs-corpus]") {
  const Bytes index = index_h265(read_bytes(ovt::test::corpus_or_skip("jctvc_CONFWIN_A_Sony_1")));
  const fb::StreamIndex& si = root_of(index);
  REQUIRE_FALSE(si.frames()->empty());
  for (const fb::FrameInfo* f : *si.frames()) {
    CAPTURE(f->id()->decode_index());
    CHECK(f->coded_size()->w() == 416);
    CHECK(f->coded_size()->h() == 240);
    CHECK(f->crop()->x() == 2);
    CHECK(f->crop()->y() == 2);
    CHECK(f->crop()->w() == 412);
    CHECK(f->crop()->h() == 236);
  }
}

// WI-1.123 AT3 (#734: the EOS is in RAP_B_Bossen_2; RAP_A_docomo_6 has none): the IRAP picture after the end of
// sequence NAL has NoRaslOutputFlag = 1, so its POC MSB is 0, the DPB holds only it, and its RASL pictures are not
// shown and skipped by the RAP map. The stream-start CRA of RAP_A_docomo_6 behaves the same.
TEST_CASE("h265_index_eos: the picture after an end of sequence starts afresh", "[index][h265][fast][needs-corpus]") {
  for (const std::string_view id : {"jctvc_RAP_B_Bossen_2", "jctvc_RAP_A_docomo_6"}) {
    CAPTURE(id);
    const Bytes index = index_h265(read_bytes(ovt::test::corpus_or_skip(id)));
    const fb::StreamIndex& si = root_of(index);
    const ovt::index::RapMap raps(si);
    // The frames that start a coded video sequence with NoRaslOutputFlag = 1: the first, and each IRAP whose
    // preceding frame ends with an end of sequence (or bitstream) unit.
    std::vector<uint32_t> fresh = {0};
    for (uint32_t d = 1; d < si.frames()->size(); ++d) {
      const auto& prev_units = *si.frames()->Get(d - 1)->units();
      const unsigned last = si.units()->Get(prev_units[prev_units.size() - 1])->type();
      if (last == 36 || last == 37) {  // EOS_NUT, EOB_NUT
        fresh.push_back(d);
      }
    }
    if (id == "jctvc_RAP_B_Bossen_2") {
      CHECK(fresh.size() >= 2);
    }
    for (const uint32_t d : fresh) {
      CAPTURE(d);
      const fb::FrameInfo& f = *si.frames()->Get(d);
      REQUIRE(f.is_rap());
      // POC MSB 0: 0 <= POC < MaxPicOrderCntLsb (at most 2^16).
      CHECK(f.poc() >= 0);
      CHECK(f.poc() < 65536);
      CHECK(to_ints(f.dpb_after()) == Ints{static_cast<int32_t>(d)});
      for (uint32_t r = d + 1; r < si.frames()->size() && !si.frames()->Get(r)->is_rap(); ++r) {
        const fb::FrameInfo& lead = *si.frames()->Get(r);
        if (is_rasl(si, lead)) {
          CAPTURE(r);
          CHECK_FALSE(lead.shown());
          CHECK(raps.skip_when_starting_at_rap(r));
        }
      }
    }
    CHECK(count_diag(si, "SYN-H265-0102", fb::Severity::Error) == 0);
  }
}
