// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the H.264 index models (chapter 7 §7.3.9, §7.3.10.1, WI-1.111b): the ten frame-boundary rules and
// field pairing, and the POC state machine for pic_order_cnt_type 0, 1 and 2 with mmco5 (ActiveSets has its own
// test file, test_index_h264_active_sets.cpp). Expected values in the synthetic cases are hand-derived from H.264
// §8.2.1 (each case says how). The [needs-corpus] cases run the whole chain (unit scan, parsers, ActiveSets,
// FrameBoundary, PocState) over the JVT conformance streams against the JM-recorded POC sequences of
// jm_poc_expected.h and skip when the corpus is absent.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "index/h264_index.h"
#include "jm_poc_expected.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/active_sets.hpp"
#include "ovt/cxx/syntax/h264/data_partition.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "test_support/corpus.h"

namespace {

namespace h264 = ovt::syntax::h264;
namespace idx = ovt::index::h264;
using ovt::syntax::Severity;
using Structure = h264::PictureStructure;

h264::NalHeader make_nal(unsigned type, unsigned ref_idc) {
  h264::NalHeader n;
  n.nal_unit_type = type;
  n.nal_ref_idc = ref_idc;
  n.IdrPicFlag = type == 5;
  return n;
}

h264::SliceHeader make_slice(uint32_t frame_num, bool idr = false) {
  h264::SliceHeader s;
  s.frame_num = frame_num;
  s.IdrPicFlag = idr;
  return s;
}

h264::SliceHeader make_field(uint32_t frame_num, bool bottom, bool idr = false) {
  h264::SliceHeader s = make_slice(frame_num, idr);
  s.field_pic_flag = true;
  s.bottom_field_flag = bottom;
  s.structure = bottom ? Structure::BottomField : Structure::TopField;
  return s;
}

void set_mmco5(h264::SliceHeader& s) {
  s.has_dec_ref_pic_marking = true;
  s.dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag = true;
  s.dec_ref_pic_marking.has_mmco5 = true;
  s.dec_ref_pic_marking.mmco.push_back({5, 0, 0, 0, 0});
}

h264::Sps make_sps(uint32_t poc_type) {
  h264::Sps s;
  s.pic_order_cnt_type = poc_type;
  s.MaxFrameNum = 16;
  s.MaxPicOrderCntLsb = 16;
  return s;
}

bool has_code(const std::vector<idx::IndexDiagnostic>& d, std::string_view code) {
  return std::ranges::any_of(d, [&](const auto& x) { return x.code == code; });
}

}  // namespace

// ---- Frame boundary (§7.3.9) ---------------------------------------------------------------------------------------

namespace {

// Two consecutive slices: returns the rules that started a picture at the second one (empty: same picture).
std::vector<uint8_t> rules_between(const h264::NalHeader& n1, const h264::SliceHeader& s1, const h264::NalHeader& n2,
                                   const h264::SliceHeader& s2, uint32_t poc_type = 0) {
  idx::FrameBoundary fb;
  const auto d1 = fb.on_slice(n1, s1, poc_type);
  REQUIRE(d1.role == idx::UnitRole::FirstSlice);
  const auto d2 = fb.on_slice(n2, s2, poc_type);
  if (d2.role == idx::UnitRole::FirstSlice) {
    REQUIRE_FALSE(d2.rules.empty());
  }
  return d2.rules;
}

}  // namespace

TEST_CASE("h264_frame_boundary: each of rules 1..9 starts a picture on its own", "[index][h264][fast]") {
  const auto p = make_nal(1, 2);
  const auto base = make_slice(3);
  auto s = base;
  CHECK(rules_between(p, base, p, s).empty());
  s.frame_num = 4;
  CHECK(rules_between(p, base, p, s) == std::vector<uint8_t>{1});
  s = base;
  s.pic_parameter_set_id = 1;
  CHECK(rules_between(p, base, p, s) == std::vector<uint8_t>{2});
  s = make_field(3, false);
  CHECK(rules_between(p, base, p, s) == std::vector<uint8_t>{3});
  CHECK(rules_between(p, make_field(3, false), p, make_field(3, true)) == std::vector<uint8_t>{4});
  CHECK(rules_between(p, base, make_nal(1, 0), base) == std::vector<uint8_t>{5});
  s = base;
  s.pic_order_cnt_lsb = 2;
  CHECK(rules_between(p, base, p, s, 0) == std::vector<uint8_t>{6});
  CHECK(rules_between(p, base, p, s, 1).empty());  // pic_order_cnt_lsb is not compared for type 1
  s = base;
  s.delta_pic_order_cnt_bottom = 1;
  CHECK(rules_between(p, base, p, s, 0) == std::vector<uint8_t>{6});
  s = base;
  s.delta_pic_order_cnt[1] = 2;
  CHECK(rules_between(p, base, p, s, 1) == std::vector<uint8_t>{7});
  CHECK(rules_between(p, base, p, s, 0).empty());
  const auto i = make_nal(5, 3);
  const auto idr = make_slice(0, true);
  CHECK(rules_between(i, idr, make_nal(1, 3), make_slice(0)) == std::vector<uint8_t>{8});
  auto idr2 = idr;
  idr2.idr_pic_id = 1;
  CHECK(rules_between(i, idr, i, idr2) == std::vector<uint8_t>{9});
}

TEST_CASE("h264_frame_boundary: rule 10, access-unit starts and first_mb_in_slice 0", "[index][h264][fast]") {
  idx::FrameBoundary fb;
  const auto p = make_nal(1, 2);
  // The first AUD and SPS open the first access unit; the first slice reports the access-unit start.
  CHECK_FALSE(fb.on_non_vcl(make_nal(9, 0)).starts_access_unit);
  CHECK(fb.on_non_vcl(make_nal(7, 3)).role == idx::UnitRole::Preceding);
  auto d = fb.on_slice(p, make_slice(0), 0);
  CHECK(d.role == idx::UnitRole::FirstSlice);
  CHECK(d.picture_index == 0);
  CHECK(d.starts_access_unit);
  CHECK(d.rules.empty());
  auto s2 = make_slice(0);
  s2.first_mb_in_slice = 2;
  d = fb.on_slice(p, s2, 0);
  CHECK(d.role == idx::UnitRole::NextSlice);
  CHECK(d.picture_index == 0);
  CHECK(d.diagnostics.empty());
  // Arbitrary slice order: a slice with first_mb_in_slice 0 that matches the open picture is only an Info.
  d = fb.on_slice(p, make_slice(0), 0);
  CHECK(d.role == idx::UnitRole::NextSlice);
  CHECK(has_code(d.diagnostics, "SYN-H264-0060"));
  CHECK(d.diagnostics[0].severity == Severity::Info);
  // An SEI after the picture starts the next access unit at the SEI; the identical slice header that follows is a new
  // picture by rule 10 alone, and does not report the access-unit start a second time.
  const auto sei = fb.on_non_vcl(make_nal(6, 0));
  CHECK(sei.starts_access_unit);
  CHECK_FALSE(fb.on_non_vcl(make_nal(8, 3)).starts_access_unit);
  d = fb.on_slice(p, s2, 0);
  CHECK(d.role == idx::UnitRole::FirstSlice);
  CHECK(d.rules == std::vector<uint8_t>{10});
  CHECK_FALSE(d.starts_access_unit);
  CHECK(d.picture_index == 1);
  // Filler data and an auxiliary slice stay with the open access unit.
  CHECK(fb.on_non_vcl(make_nal(12, 0)).role == idx::UnitRole::Following);
  CHECK(fb.on_non_vcl(make_nal(19, 0)).role == idx::UnitRole::Following);
  CHECK(fb.on_slice(p, s2, 0).role == idx::UnitRole::NextSlice);
}

TEST_CASE("h264_frame_boundary: prefix NAL, redundant pictures, partitions and extension slices",
          "[index][h264][fast]") {
  idx::FrameBoundary fb;
  const auto p = make_nal(1, 2);
  REQUIRE(fb.on_slice(p, make_slice(0), 0).role == idx::UnitRole::FirstSlice);
  // A prefix NAL before the next slice of the same picture does not split it (SPEC-ASSUMPTION #568).
  const auto pre = fb.on_non_vcl(make_nal(14, 2));
  CHECK(pre.role == idx::UnitRole::Prefix);
  CHECK_FALSE(pre.starts_access_unit);
  auto s = make_slice(0);
  s.first_mb_in_slice = 1;
  CHECK(fb.on_slice(p, s, 0).role == idx::UnitRole::NextSlice);
  // Non-base views and partitions B / C join the open picture.
  CHECK(fb.on_slice(make_nal(20, 2), make_slice(0), 0).role == idx::UnitRole::ExtensionSlice);
  auto bc = fb.on_slice_without_header(make_nal(3, 2));
  CHECK(bc.role == idx::UnitRole::PartitionBC);
  CHECK(bc.picture_index == 0);
  // A redundant coded picture belongs to the primary one even though its header differs.
  auto r = make_slice(5);
  r.redundant_pic_cnt = 1;
  const auto rd = fb.on_slice(p, r, 0);
  CHECK(rd.role == idx::UnitRole::RedundantSlice);
  CHECK(rd.picture_index == 0);
  // A prefix NAL before a new picture: the slice decides.
  (void)fb.on_non_vcl(make_nal(14, 2));
  const auto d = fb.on_slice(p, make_slice(1), 0);
  CHECK(d.role == idx::UnitRole::FirstSlice);
  CHECK(d.starts_access_unit);
  CHECK(d.rules == std::vector<uint8_t>{1});
  // End of sequence: the next slice starts a picture even with an identical header.
  CHECK(fb.on_non_vcl(make_nal(10, 0)).role == idx::UnitRole::Following);
  const auto after = fb.on_slice(p, make_slice(1), 0);
  CHECK(after.role == idx::UnitRole::FirstSlice);
  CHECK(after.rules.empty());
  CHECK(after.picture_index == 2);
  // End of sequence, then a PPS: the PPS starts the next access unit (it must not be held back as a change inside the
  // closed picture; fuzz-nightly h264_index crash 4e2718fa, 2026-11-19), and the slice does not report it again.
  CHECK(fb.on_non_vcl(make_nal(10, 0)).role == idx::UnitRole::Following);
  CHECK(fb.on_non_vcl(make_nal(8, 3)).starts_access_unit);
  CHECK_FALSE(fb.on_non_vcl(make_nal(7, 3)).starts_access_unit);
  const auto next = fb.on_slice(p, make_slice(1), 0);
  CHECK(next.role == idx::UnitRole::FirstSlice);
  CHECK_FALSE(next.starts_access_unit);
  CHECK(next.picture_index == 3);
  // A redundant slice after an SPS that started the next access unit is a new picture (rule 10), not part of the open
  // one, so a PPS that follows it starts the access unit after it (fuzz-nightly h264_index crash fca46b18).
  CHECK(fb.on_non_vcl(make_nal(7, 3)).starts_access_unit);
  auto redundant = make_slice(1);
  redundant.redundant_pic_cnt = 1;
  const auto late = fb.on_slice(p, redundant, 0);
  CHECK(late.role == idx::UnitRole::FirstSlice);
  CHECK(late.rules == std::vector<uint8_t>{10});
  CHECK(late.picture_index == 4);
  CHECK(fb.on_non_vcl(make_nal(8, 3)).starts_access_unit);
}

TEST_CASE("h264_frame_boundary: complementary field pairs", "[index][h264][fast]") {
  idx::FrameBoundary fb;
  const auto ref = make_nal(1, 2);
  const auto nonref = make_nal(1, 0);
  // IDR top + non-IDR bottom with the same frame_num: a pair (the second field is not an IDR).
  auto d = fb.on_slice(make_nal(5, 3), make_field(0, false, true), 0);
  CHECK(d.structure == Structure::TopField);
  CHECK(d.second_field_of == -1);
  d = fb.on_slice(ref, make_field(0, true), 0);
  CHECK(d.structure == Structure::BottomField);
  CHECK(d.second_field_of == 0);
  // A third field with the same frame_num and opposite parity of the last one does not pair (already paired).
  d = fb.on_slice(ref, make_field(0, false), 0);
  CHECK(d.second_field_of == -1);
  // Same parity twice: no pair.
  d = fb.on_slice(ref, make_field(1, false), 0);
  CHECK(d.second_field_of == -1);
  // Different frame_num: no pair.
  d = fb.on_slice(ref, make_field(2, true), 0);
  CHECK(d.second_field_of == -1);
  // Reference then non-reference field: not a complementary pair (H.264 3.29 / 3.30, as JM).
  d = fb.on_slice(ref, make_field(3, false), 0);
  d = fb.on_slice(nonref, make_field(3, true), 0);
  CHECK(d.second_field_of == -1);
  // Two non-reference fields: a complementary non-reference pair, bottom first.
  d = fb.on_slice(nonref, make_field(4, true), 0);
  const int32_t first = d.picture_index;
  d = fb.on_slice(nonref, make_field(4, false), 0);
  CHECK(d.second_field_of == first);
  // A non-IDR first field followed by an IDR field: no pair.
  d = fb.on_slice(ref, make_field(5, false), 0);
  d = fb.on_slice(make_nal(5, 3), make_field(5, true, true), 0);
  CHECK(d.second_field_of == -1);
}

// ---- POC (§7.3.10.1) -----------------------------------------------------------------------------------------------

TEST_CASE("h264_poc: type 0 wrap-around, and a non-reference picture's lsb is never remembered",
          "[index][h264][fast]") {
  idx::PocState st;
  const auto sps = make_sps(0);  // MaxPicOrderCntLsb 16, MaxLsb / 2 = 8
  CHECK(st.step(make_nal(5, 3), make_slice(0, true), sps).poc == 0);
  auto s = make_slice(1);
  s.pic_order_cnt_lsb = 6;
  CHECK(st.step(make_nal(1, 2), s, sps).poc == 6);
  s.pic_order_cnt_lsb = 12;
  CHECK(st.step(make_nal(1, 2), s, sps).poc == 12);
  // Non-reference at lsb 2: 12 − 2 = 10 >= 8, so PicOrderCntMsb = 0 + 16 → 18.
  s.pic_order_cnt_lsb = 2;
  auto r = st.step(make_nal(1, 0), s, sps);
  CHECK(r.poc == 18);
  CHECK(r.pic_order_cnt_msb == 16);
  // Reference at lsb 10, derived from the reference at (msb 0, lsb 12) → 10. Had the non-reference picture been
  // remembered (msb 16, lsb 2): 10 − 2 = 8 is not > 8 → msb 16 → 26.
  s.pic_order_cnt_lsb = 10;
  r = st.step(make_nal(1, 2), s, sps);
  CHECK(r.prev_pic_order_cnt_msb == 0);
  CHECK(r.prev_pic_order_cnt_lsb == 12);
  CHECK(r.poc == 10);
  // lsb 1 after 10: 9 >= 8 → msb 16 → 17. Then lsb 15 after (16, 1): 14 > 8 → msb 0 → 15.
  s.pic_order_cnt_lsb = 1;
  CHECK(st.step(make_nal(1, 2), s, sps).poc == 17);
  s.pic_order_cnt_lsb = 15;
  CHECK(st.step(make_nal(1, 2), s, sps).poc == 15);
  // A first picture lsb 14 after the IDR goes backwards: 14 − 0 > 8 → msb −16 → −2.
  (void)st.step(make_nal(5, 3), make_slice(0, true), sps);
  s.pic_order_cnt_lsb = 14;
  CHECK(st.step(make_nal(1, 2), s, sps).poc == -2);
}

TEST_CASE("h264_poc: type 0 frames and fields, delta_pic_order_cnt_bottom", "[index][h264][fast]") {
  idx::PocState st;
  const auto sps = make_sps(0);
  auto s = make_slice(0, true);
  s.delta_pic_order_cnt_bottom = -1;  // bottom field first in time: PicOrderCnt = Min(0, −1) = −1
  auto r = st.step(make_nal(5, 3), s, sps);
  CHECK(r.top_field_order_cnt == 0);
  CHECK(r.bottom_field_order_cnt == -1);
  CHECK(r.poc == -1);
  CHECK(r.structure == Structure::Frame);
  auto t = make_field(1, false);
  t.pic_order_cnt_lsb = 4;
  r = st.step(make_nal(1, 2), t, sps);
  CHECK(r.has_top);
  CHECK_FALSE(r.has_bottom);
  CHECK(r.poc == 4);
  auto b = make_field(1, true);
  b.pic_order_cnt_lsb = 5;
  r = st.step(make_nal(1, 2), b, sps);
  CHECK(r.structure == Structure::BottomField);
  CHECK(r.bottom_field_order_cnt == 5);
  CHECK(r.poc == 5);
}

TEST_CASE("h264_poc: type 0 mmco5 resets prevPicOrderCntMsb and uses the adjusted TopFieldOrderCnt",
          "[index][h264][fast]") {
  idx::PocState st;
  const auto sps = make_sps(0);
  (void)st.step(make_nal(5, 3), make_slice(0, true), sps);
  // Reference frames at lsb 6 and 12, then an mmco5 frame at lsb 2 → msb 16, POC 18.
  auto s = make_slice(1);
  s.pic_order_cnt_lsb = 6;
  (void)st.step(make_nal(1, 2), s, sps);
  s.pic_order_cnt_lsb = 12;
  (void)st.step(make_nal(1, 2), s, sps);
  s = make_slice(2);
  s.pic_order_cnt_lsb = 2;
  s.delta_pic_order_cnt_bottom = 1;
  set_mmco5(s);
  auto r = st.step(make_nal(1, 2), s, sps);
  CHECK(r.poc == 18);  // Top 18, Bottom 19
  CHECK(r.mmco5);
  CHECK(r.top_after_mmco5 == 0);  // 18 − Min(18, 19)
  CHECK(r.bottom_after_mmco5 == 1);
  // Next picture: prevPicOrderCntMsb = 0, prevPicOrderCntLsb = adjusted TopFieldOrderCnt = 0.
  s = make_slice(1);
  s.pic_order_cnt_lsb = 4;
  r = st.step(make_nal(1, 2), s, sps);
  CHECK(r.prev_pic_order_cnt_msb == 0);
  CHECK(r.prev_pic_order_cnt_lsb == 0);
  CHECK(r.poc == 4);
  // An mmco5 bottom field: prevPicOrderCntLsb = 0.
  auto b = make_field(2, true);
  b.pic_order_cnt_lsb = 9;
  set_mmco5(b);
  r = st.step(make_nal(1, 2), b, sps);
  CHECK(r.poc == 9);
  CHECK(r.bottom_after_mmco5 == 0);
  s = make_slice(1);
  s.pic_order_cnt_lsb = 15;
  r = st.step(make_nal(1, 2), s, sps);
  CHECK(r.prev_pic_order_cnt_lsb == 0);
  CHECK(r.poc == -1);  // 15 − 0 > 8 → msb −16
  // An mmco5 top field whose TopFieldOrderCnt is adjusted to 0: prevPicOrderCntLsb = 0 as well.
  auto t = make_field(2, false);
  t.pic_order_cnt_lsb = 3;
  set_mmco5(t);
  r = st.step(make_nal(1, 2), t, sps);
  CHECK(r.top_after_mmco5 == 0);
}

TEST_CASE("h264_poc: type 1 expected order counts, non-reference offset and frame_num wrap", "[index][h264][fast]") {
  idx::PocState st;
  auto sps = make_sps(1);
  // MR9_BT_B's cycle: one reference frame per cycle, offset_for_ref_frame[0] = 6, offset_for_non_ref_pic = −2,
  // offset_for_top_to_bottom_field = 1.
  sps.num_ref_frames_in_pic_order_cnt_cycle = 1;
  sps.offset_for_ref_frame = {6};
  sps.ExpectedDeltaPerPicOrderCntCycle = 6;
  sps.offset_for_non_ref_pic = -2;
  sps.offset_for_top_to_bottom_field = 1;
  auto r = st.step(make_nal(5, 3), make_slice(0, true), sps);
  CHECK(r.top_field_order_cnt == 0);
  CHECK(r.bottom_field_order_cnt == 1);
  CHECK(r.poc == 0);
  // frame_num 1, reference: absFrameNum 1 → expected = 0 * 6 + 6 = 6.
  r = st.step(make_nal(1, 2), make_slice(1), sps);
  CHECK(r.poc == 6);
  // frame_num 2, non-reference: absFrameNum 2 − 1 = 1 → 6 − 2 = 4; delta_pic_order_cnt[0] = −2 → 2.
  auto s = make_slice(2);
  s.delta_pic_order_cnt[0] = -2;
  r = st.step(make_nal(1, 0), s, sps);
  CHECK(r.poc == 2);
  // A bottom field: expected + offset_for_top_to_bottom_field + delta_pic_order_cnt[0]: frame_num 2 ref → 12 + 1.
  r = st.step(make_nal(1, 2), make_field(2, true), sps);
  CHECK(r.poc == 13);
  // frame_num wraps 15 → 0: FrameNumOffset += MaxFrameNum (16).
  (void)st.step(make_nal(1, 2), make_slice(15), sps);
  r = st.step(make_nal(1, 2), make_slice(0), sps);
  CHECK(r.prev_frame_num_offset == 0);
  CHECK(r.frame_num_offset == 16);
  CHECK(r.poc == 96);  // absFrameNum 16 → 15 * 6 + 6
  // num_ref_frames_in_pic_order_cnt_cycle 0: expectedPicOrderCnt is 0 (plus the non-reference offset).
  sps.num_ref_frames_in_pic_order_cnt_cycle = 0;
  sps.offset_for_ref_frame.clear();
  r = st.step(make_nal(1, 0), make_slice(1), sps);
  CHECK(r.poc == -2);
}

TEST_CASE("h264_poc: type 1 mmco5 sets prevFrameNumOffset and prevFrameNum to 0", "[index][h264][fast]") {
  idx::PocState st;
  auto sps = make_sps(1);
  sps.num_ref_frames_in_pic_order_cnt_cycle = 1;
  sps.offset_for_ref_frame = {1};
  sps.ExpectedDeltaPerPicOrderCntCycle = 1;
  // sp1_bt_a's shape: frame_num 0..10, mmco5 at frame_num 10 (POC 10 + delta 1 = 11), then frame_num 1 → POC 1.
  (void)st.step(make_nal(5, 3), make_slice(0, true), sps);
  for (uint32_t f = 1; f < 10; ++f) {
    CHECK(st.step(make_nal(1, 2), make_slice(f), sps).poc == static_cast<int32_t>(f));
  }
  auto s = make_slice(10);
  s.delta_pic_order_cnt[0] = 1;
  set_mmco5(s);
  auto r = st.step(make_nal(1, 2), s, sps);
  CHECK(r.poc == 11);
  CHECK(r.top_after_mmco5 == 0);
  r = st.step(make_nal(1, 2), make_slice(1), sps);
  CHECK(r.prev_frame_num_offset == 0);
  CHECK(r.frame_num_offset == 0);  // prevFrameNum 0 is not > 1
  CHECK(r.poc == 1);
}

TEST_CASE("h264_poc: type 2 order counts follow decoding order", "[index][h264][fast]") {
  idx::PocState st;
  const auto sps = make_sps(2);
  CHECK(st.step(make_nal(5, 3), make_slice(0, true), sps).poc == 0);
  CHECK(st.step(make_nal(1, 2), make_slice(1), sps).poc == 2);
  CHECK(st.step(make_nal(1, 0), make_slice(2), sps).poc == 3);  // 2 * (0 + 2) − 1
  auto r = st.step(make_nal(1, 2), make_field(2, false), sps);
  CHECK(r.poc == 4);
  r = st.step(make_nal(1, 2), make_field(2, true), sps);
  CHECK(r.poc == 4);
  // Wrap 15 → 0 and an mmco5 picture.
  (void)st.step(make_nal(1, 2), make_slice(15), sps);
  r = st.step(make_nal(1, 2), make_slice(0), sps);
  CHECK(r.poc == 32);
  auto s = make_slice(1);
  set_mmco5(s);
  r = st.step(make_nal(1, 2), s, sps);
  CHECK(r.poc == 34);
  CHECK(r.top_after_mmco5 == 0);
  r = st.step(make_nal(1, 2), make_slice(1), sps);
  CHECK(r.poc == 2);
}

TEST_CASE("h264_poc: crafted streams cannot overflow the order counts", "[index][h264][fast]") {
  idx::PocState st;
  auto sps = make_sps(1);
  sps.MaxFrameNum = 65536;
  sps.num_ref_frames_in_pic_order_cnt_cycle = 1;
  sps.offset_for_ref_frame = {INT32_MAX};
  sps.ExpectedDeltaPerPicOrderCntCycle = INT32_MAX;
  sps.offset_for_non_ref_pic = INT32_MIN;
  sps.offset_for_top_to_bottom_field = INT32_MAX;
  (void)st.step(make_nal(5, 3), make_slice(0, true), sps);
  bool clamped = false;
  for (int i = 0; i < 20000; ++i) {
    auto s = make_slice(i % 2 == 0 ? 65535U : 0U);
    s.delta_pic_order_cnt = {INT32_MAX, INT32_MIN};
    const auto r = st.step(make_nal(1, (i % 3) != 0 ? 2U : 0U), s, sps);
    clamped = clamped || r.poc_clamped;
    REQUIRE(r.poc <= idx::kPocLimit);
    REQUIRE(r.poc >= -idx::kPocLimit);
  }
  CHECK(clamped);
  idx::PocState st0;
  auto sps0 = make_sps(0);
  sps0.MaxPicOrderCntLsb = 65536;
  for (int i = 0; i < 40000; ++i) {
    auto s = make_slice(1);
    s.pic_order_cnt_lsb = static_cast<uint32_t>((i % 2) * 40000);
    s.delta_pic_order_cnt_bottom = INT32_MAX;
    const auto r = st0.step(make_nal(1, 2), s, sps0);
    REQUIRE(r.top_field_order_cnt <= idx::kPocLimit);
  }
}

// ---- conformance streams: the whole chain (needs the corpus) ------------------------------------------------------

namespace {

// Units -> parsers -> ActiveSets -> FrameBoundary -> PocState, as WI-1.121 will wire them.
class Chain {
 public:
  struct Picture {
    h264::NalHeader nal;
    h264::SliceHeader sh;
    idx::BoundaryDecision bd;
    idx::PocResult pr;
  };

  void add(std::span<const uint8_t> nal_bytes) {
    const std::vector<uint8_t> nal(nal_bytes.begin(), nal_bytes.end());
    const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
    ovt::BitReader br(rbsp.rbsp);
    ovt::syntax::SyntaxBuilder b(br);
    b.set_recording(false);
    const h264::NalHeader h = h264::parse_nal_header(br, b);
    const uint64_t unit = unit_++;
    const uint32_t t = h.nal_unit_type;
    if (!idx::is_vcl(t)) {
      const idx::BoundaryDecision bd = fb_.on_non_vcl(h);
      if (bd.starts_access_unit) {
        sets_.end_picture();
      }
      if (t == 7) {
        sets_.put_sps(h264::parse_sps(br, b), rbsp.rbsp, unit, set_diags);
      } else if (t == 8) {
        const h264::Pps pps = h264::parse_pps(br, b, sets_.parameter_sets(set_diags).sps);
        sets_.put_pps(pps, rbsp.rbsp, unit, set_diags);
      } else if (t == 15) {
        sets_.put_subset_sps(h264::parse_seq_parameter_set_data(br, b), rbsp.rbsp, unit, set_diags);
      }
      return;
    }
    if (t == 3 || t == 4) {
      (void)fb_.on_slice_without_header(h);
      return;
    }
    const h264::ParameterSets& ps = sets_.parameter_sets(set_diags);
    std::optional<h264::SliceHeader> sh;
    if (t == 2) {
      sh = h264::parse_slice_data_partition_a(br, b, h, ps).slice_header;
    } else {
      sh = h264::parse_slice_layer(br, b, h, ps);
    }
    if (!sh) {
      (void)fb_.on_slice_without_header(h);
      return;
    }
    const h264::Pps* pps = ps.pps.find(sh->pic_parameter_set_id);
    REQUIRE(pps != nullptr);
    const h264::Sps* sps = ps.sps.find(pps->seq_parameter_set_id);
    REQUIRE(sps != nullptr);
    const idx::BoundaryDecision bd = fb_.on_slice(h, *sh, sps->pic_order_cnt_type);
    const bool first = bd.role == idx::UnitRole::FirstSlice;
    const auto act = sets_.activate(h, *sh, first, set_diags);
    REQUIRE(act.has_value());
    if (!first) {
      return;
    }
    Picture p;
    p.nal = h;
    p.sh = *sh;
    p.bd = bd;
    p.pr = poc_.step(h, *sh, *act->sps);
    pictures.push_back(std::move(p));
  }

  void add_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::vector<uint8_t>> units;
    ovt::syntax::scan_annexb(std::span<const uint8_t>(data), 0, ovt::syntax::kNoContainerSample,
                             [&units](const ovt::syntax::ScannedUnit& u) {
                               if (!u.bytes.empty()) {
                                 units.emplace_back(u.bytes.begin(), u.bytes.end());
                               }
                             });
    for (const auto& u : units) {
      add(u);
    }
  }

  // The pictures as jm_poc_expected.h tokens.
  [[nodiscard]] std::vector<std::string> tokens() const {
    std::vector<std::string> out;
    for (const Picture& p : pictures) {
      const char s = p.pr.structure == Structure::TopField ? 'T' : p.pr.structure == Structure::BottomField ? 'B' : 'F';
      out.push_back(s + std::to_string(p.pr.poc));
    }
    return out;
  }

  std::vector<Picture> pictures;
  std::vector<h264::SetDiagnostic> set_diags;

 private:
  h264::ActiveSets sets_;
  idx::FrameBoundary fb_;
  idx::PocState poc_;
  uint64_t unit_ = 0;
};

std::vector<std::string> split(std::string_view text) {
  std::istringstream in{std::string(text)};
  std::vector<std::string> out;
  std::string tok;
  while (in >> tok) {
    out.push_back(tok);
  }
  return out;
}

// Indexes the stream and compares every picture's structure and PicOrderCnt with the JM recording, in decode order.
Chain check_against_jm(std::string_view id, std::string_view expected) {
  Chain c;
  c.add_file(ovt::test::corpus_or_skip(id));
  const std::vector<std::string> jm = split(expected);
  const std::vector<std::string> ours = c.tokens();
  CHECK(ours.size() == jm.size());
  for (size_t i = 0; i < jm.size() && i < ours.size(); ++i) {
    CAPTURE(id, i);
    CHECK(ours[i] == jm[i]);
  }
  CHECK(c.set_diags.empty());
  return c;
}

}  // namespace

// The hand-made streams of core/tests/fuzz/corpus/h264_index (needs no corpus); expected values hand-derived:
//  seed 0, type 0 (MaxPicOrderCntLsb 16): IDR lsb 0, P lsb 4, non-reference lsb 2, mmco5 reference lsb 8, then lsb 2
//          derived from prevPicOrderCntLsb = 8 − 8 = 0 → 2.
//  seed 1, type 1 fields (cycle {4}, offset_for_non_ref_pic −2, offset_for_top_to_bottom_field 1): IDR top 0,
//          bottom 0 + 1, frame_num 1 top 4, bottom 5, a non-reference frame at frame_num 2 with
//          delta_pic_order_cnt[0] = −1: expected 4 − 2 = 2, Top 1, Bottom 2 → 1; then end of sequence.
//  seed 2, type 2: 0, 2, non-reference 3, a repeated PPS (rule 10) then reference frame_num 2 → 4 with a second slice
//  that
//          continues the picture (first_mb_in_slice 0: Info SYN-H264-0060).
TEST_CASE("h264_chain: the synthetic streams of the fuzz seeds", "[index][h264][fast]") {
  const std::filesystem::path dir = OVT_H264_INDEX_SEEDS;
  Chain c0;
  c0.add_file(dir / "seed_0_poc_type_0.bin");
  CHECK(c0.tokens() == std::vector<std::string>{"F0", "F4", "F2", "F8", "F2"});
  REQUIRE(c0.pictures.size() == 5);
  CHECK(c0.pictures[3].pr.mmco5);
  CHECK(c0.pictures[4].pr.prev_pic_order_cnt_lsb == 0);
  CHECK(c0.set_diags.empty());
  Chain c1;
  c1.add_file(dir / "seed_1_poc_type_1.bin");
  CHECK(c1.tokens() == std::vector<std::string>{"T0", "B1", "T4", "B5", "F1"});
  REQUIRE(c1.pictures.size() == 5);
  CHECK(c1.pictures[1].bd.second_field_of == 0);
  CHECK(c1.pictures[3].bd.second_field_of == 2);
  CHECK(c1.pictures[4].bd.second_field_of == -1);
  Chain c2;
  c2.add_file(dir / "seed_2_poc_type_2.bin");
  CHECK(c2.tokens() == std::vector<std::string>{"F0", "F2", "F3", "F4"});
  REQUIRE(c2.pictures.size() == 4);
  CHECK(c2.pictures[3].bd.rules == std::vector<uint8_t>{5, 10});  // reference after a non-reference picture
  CHECK(c2.set_diags.empty());
}

TEST_CASE("h264_poc_unit: the JM recordings parse into the expected picture counts", "[index][h264][fast]") {
  // Guards the expected tables themselves (needs no corpus): JM's "N frames are decoded" for each stream.
  namespace e = ovt::test::h264;
  CHECK(split(e::kJm_CVPCMNL1_SVA_C).size() == 30);
  CHECK(split(e::kJm_MR6_BT_B).size() == 120);
  CHECK(split(e::kJm_MR9_BT_B).size() == 64);
  CHECK(split(e::kJm_CVFI1_Sony_D).size() == 34);
  CHECK(split(e::kJm_sp1_bt_a).size() == 400);
}

// AT1: CVPCMNL1_SVA_C (pic_order_cnt_type 0, frames) and MR6_BT_B (pic_order_cnt_type 1, field pairs) against JM.
TEST_CASE("h264_poc_unit: CVPCMNL1_SVA_C and MR6_BT_B per-picture POC equal JM", "[index][h264][fast][needs-corpus]") {
  namespace e = ovt::test::h264;
  const Chain a = check_against_jm("jvt_CVPCMNL1_SVA_C", e::kJm_CVPCMNL1_SVA_C);
  REQUIRE_FALSE(a.pictures.empty());
  (void)check_against_jm("jvt_MR6_BT_B", e::kJm_MR6_BT_B);
}

// MR9_BT_B mixes frames, MBAFF frames and field pairs with pic_order_cnt_type 1 (the stream of the spec's AT2, which
// has no mmco5, issue #568): its POC sequence is checked here as an extra AT1 stream.
TEST_CASE("h264_poc_unit: MR9_BT_B per-picture POC equal JM", "[index][h264][fast][needs-corpus]") {
  (void)check_against_jm("jvt_MR9_BT_B", ovt::test::h264::kJm_MR9_BT_B);
}

// AT2 (SPEC-ASSUMPTION #568: sp1_bt_a, the only JVT stream in the corpus with mmco5, replaces MR9_BT_B): the
// picture after each mmco5 picture starts from prevFrameNumOffset 0 (type 1; prevPicOrderCntMsb for type 0) and
// every POC equals JM's.
TEST_CASE("h264_poc_mmco5: sp1_bt_a POC after mmco5 equals JM", "[index][h264][slow][needs-corpus]") {
  const Chain c = check_against_jm("jvt_sp1_bt_a", ovt::test::h264::kJm_sp1_bt_a);
  size_t mmco5 = 0;
  for (size_t i = 0; i + 1 < c.pictures.size(); ++i) {
    if (!c.pictures[i].pr.mmco5) {
      continue;
    }
    ++mmco5;
    CAPTURE(i);
    const idx::PocResult& next = c.pictures[i + 1].pr;
    CHECK(next.prev_frame_num_offset == 0);
    CHECK(next.prev_pic_order_cnt_msb == 0);
  }
  CHECK(mmco5 == 39);  // the Mini's trace_headers scan: 39 mmco5 operations
}

// AT3: CVFI1_Sony_D (field pictures): one FrameInfo per field, as many as JM decoded, each Top or Bottom, and every
// second field paired with the field before it.
TEST_CASE("h264_poc_fields: CVFI1_Sony_D has one picture per field", "[index][h264][slow][needs-corpus]") {
  const Chain c = check_against_jm("jvt_CVFI1_Sony_D", ovt::test::h264::kJm_CVFI1_Sony_D);
  REQUIRE(c.pictures.size() == 34);
  for (size_t i = 0; i < c.pictures.size(); ++i) {
    CAPTURE(i);
    const idx::BoundaryDecision& bd = c.pictures[i].bd;
    CHECK(bd.picture_index == static_cast<int32_t>(i));
    CHECK(bd.structure == (i % 2 == 0 ? Structure::TopField : Structure::BottomField));
    CHECK(bd.second_field_of == (i % 2 == 0 ? -1 : static_cast<int32_t>(i) - 1));
  }
}
