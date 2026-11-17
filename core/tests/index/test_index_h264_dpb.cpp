// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the H.264 DPB simulation of the StreamIndex (chapter 7 §7.4.4, WI-1.122): picture numbers, initial
// reference lists for frames and fields (H.264 §8.2.4.2), their modification (§8.2.4.3), the sliding window and the
// memory management control operations (§8.2.5) and frame_num gaps (§8.2.5.2). The synthetic cases drive
// ovt::index::h264::Dpb with hand-made headers; each case says how its expected values follow from the standard.
// The [needs-corpus] cases run the H.264 dispatch of the builder over the JVT conformance streams:
//   h264_reflist_vs_jm   (acceptance test 1) the first slice's RefPicList0 / RefPicList1, as POCs, equal the lists
//                        JM 19.0 builds (tests/golden/h264/<id>.jm-reflist.txt.gz, recorded with
//                        tests/oracles/patches/jm-reflist-print.patch, docs/acceptance/wi/WI-1.122.md), and
//                        declared_refs is the set of frame stores those lists name;
//   dpb bounded          (acceptance test 2) MR6_BT_B and MR9_BT_B: dpb_after never exceeds max_num_ref_frames and
//                        no SYN-H264-0105 / 0106;
//   frame_num gaps       (acceptance test 3) MR1_BT_A: non-existing frames are inserted with SYN-H264-0102.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "index/builder_internal.h"
#include "index/h264_index.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "test_support/corpus.h"

namespace {

namespace h264 = ovt::syntax::h264;
namespace idx = ovt::index;
using idx::h264::Dpb;
using idx::h264::DpbStepResult;
using idx::h264::Parity;

// ---- synthetic pictures -----------------------------------------------------------------------------------------

h264::SliceHeader slice(h264::SliceType type, uint32_t frame_num, uint32_t l0 = 0, uint32_t l1 = 0) {
  h264::SliceHeader s;
  s.slice_type_mod5 = type;
  s.frame_num = frame_num;
  s.num_ref_idx_l0_active_minus1 = l0;
  s.num_ref_idx_l1_active_minus1 = l1;
  return s;
}

h264::SliceHeader idr(uint32_t frame_num = 0) {
  h264::SliceHeader s = slice(h264::kSliceI, frame_num);
  s.IdrPicFlag = true;
  s.has_dec_ref_pic_marking = true;
  s.dec_ref_pic_marking.idr = true;
  return s;
}

h264::SliceHeader as_field(h264::SliceHeader s, bool bottom) {
  s.field_pic_flag = true;
  s.bottom_field_flag = bottom;
  s.structure = bottom ? h264::PictureStructure::BottomField : h264::PictureStructure::TopField;
  return s;
}

void add_mmco(h264::SliceHeader& s, h264::MmcoOp op) {
  s.has_dec_ref_pic_marking = true;
  s.dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag = true;
  s.dec_ref_pic_marking.has_mmco5 = s.dec_ref_pic_marking.has_mmco5 || op.memory_management_control_operation == 5;
  s.dec_ref_pic_marking.mmco.push_back(op);
}

void add_modification(h264::SliceHeader& s, size_t list, uint32_t idc, uint32_t value) {
  h264::RefPicListModification& m = s.ref_pic_list_modification;
  h264::RefPicListModificationOp op;
  op.modification_of_pic_nums_idc = idc;
  (idc == 2 ? op.long_term_pic_num : op.abs_diff_pic_num_minus1) = value;
  (list == 0 ? m.ref_pic_list_modification_flag_l0 : m.ref_pic_list_modification_flag_l1) = true;
  (list == 0 ? m.ops_l0 : m.ops_l1).push_back(op);
}

// Feeds pictures to one Dpb in decode order; decode indices count from 0.
class Sim {
 public:
  explicit Sim(uint32_t max_num_ref_frames, bool gaps_allowed = false) {
    sps_.max_num_ref_frames = max_num_ref_frames;
    sps_.MaxFrameNum = 16;
    sps_.gaps_in_frame_num_value_allowed_flag = gaps_allowed;
  }

  // A frame with TopFieldOrderCnt = BottomFieldOrderCnt = poc, or a field with that order count.
  DpbStepResult add(const h264::SliceHeader& sh, int32_t poc, unsigned nal_ref_idc = 1, int32_t second_field_of = -1) {
    h264::NalHeader nal;
    nal.nal_unit_type = sh.IdrPicFlag ? 5U : 1U;
    nal.nal_ref_idc = nal_ref_idc;
    nal.IdrPicFlag = sh.IdrPicFlag;
    idx::h264::PocResult pr;
    pr.poc = poc;
    pr.top_field_order_cnt = poc;
    pr.bottom_field_order_cnt = poc;
    pr.has_top = !sh.field_pic_flag || !sh.bottom_field_flag;
    pr.has_bottom = !sh.field_pic_flag || sh.bottom_field_flag;
    for (const h264::MmcoOp& op : sh.dec_ref_pic_marking.mmco) {
      pr.mmco5 = pr.mmco5 || op.memory_management_control_operation == 5;
    }
    return dpb_.step(next_++, nal, sh, sps_, pr, second_field_of);
  }

  Dpb& dpb() { return dpb_; }

 private:
  h264::Sps sps_;
  Dpb dpb_;
  int32_t next_ = 0;
};

std::vector<int32_t> pocs(const std::vector<idx::h264::RefPic>& list) {
  std::vector<int32_t> out;
  out.reserve(list.size());
  for (const auto& r : list) {
    out.push_back(r.poc);
  }
  return out;
}

std::vector<int32_t> decode_indices(const std::vector<idx::h264::RefPic>& list) {
  std::vector<int32_t> out;
  out.reserve(list.size());
  for (const auto& r : list) {
    out.push_back(r.decode_index);
  }
  return out;
}

bool has_code(const DpbStepResult& r, std::string_view code) {
  return std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                     [code](const idx::h264::IndexDiagnostic& d) { return d.code == code; });
}

using V = std::vector<int32_t>;

TEST_CASE("h264_dpb: P frames, picture numbers and the sliding window", "[index][h264][dpb][fast]") {
  // max_num_ref_frames = 2. RefPicList0 of a P frame is the short-term frames by PicNum (= FrameNumWrap)
  // descending (§8.2.4.2.1); once two frames are references the one with the smallest FrameNumWrap is dropped
  // (§8.2.5.3).
  Sim s(2);
  CHECK(s.add(idr(), 0).dpb_after == V{0});
  DpbStepResult r = s.add(slice(h264::kSliceP, 1), 2);
  CHECK(decode_indices(r.ref_pic_list[0]) == V{0});
  CHECK(r.dpb_after == V{0, 1});
  r = s.add(slice(h264::kSliceP, 2, 1), 4);
  CHECK(decode_indices(r.ref_pic_list[0]) == V{1, 0});
  CHECK(r.dpb_after == V{1, 2});
  r = s.add(slice(h264::kSliceP, 3, 1), 6);
  CHECK(decode_indices(r.ref_pic_list[0]) == V{2, 1});
  CHECK(r.declared_refs == V{2, 1});
  CHECK(r.dpb_after == V{2, 3});
  // A non-reference picture is no store and leaves the DPB as it is.
  r = s.add(slice(h264::kSliceP, 4, 1), 8, 0);
  CHECK(r.dpb_after == V{2, 3});
  CHECK(r.diagnostics.empty());
}

TEST_CASE("h264_dpb: FrameNumWrap across the frame_num wrap", "[index][h264][dpb][fast]") {
  // MaxFrameNum 16: after frame_num 14 and 15 the frame with frame_num 0 has FrameNumWrap 0 and the earlier ones
  // -2 and -1 (eq. 8-27), so frame 15 comes first and the window drops frame 14.
  Sim s(2);
  (void)s.add(idr(14), 0);
  (void)s.add(slice(h264::kSliceP, 15), 2);
  const DpbStepResult r = s.add(slice(h264::kSliceP, 0, 1), 4);
  CHECK(decode_indices(r.ref_pic_list[0]) == V{1, 0});
  CHECK(r.dpb_after == V{1, 2});
}

TEST_CASE("h264_dpb: B frames ordered by POC, and the swap of an equal RefPicList1", "[index][h264][dpb][fast]") {
  // §8.2.4.2.3: L0 = POC below the current descending, then above ascending; L1 the other way round.
  Sim s(4);
  (void)s.add(idr(), 0);                                         // d0 POC 0
  (void)s.add(slice(h264::kSliceP, 1), 8);                       // d1 POC 8
  (void)s.add(slice(h264::kSliceB, 2, 1, 1), 4);                 // d2 POC 4, reference B
  DpbStepResult r = s.add(slice(h264::kSliceB, 3, 2, 2), 2, 0);  // d3 POC 2, non-reference
  CHECK(pocs(r.ref_pic_list[0]) == V{0, 4, 8});
  CHECK(pocs(r.ref_pic_list[1]) == V{4, 8, 0});
  CHECK(r.declared_refs == V{0, 2, 1});
  // POC 10 is after every reference: both lists would be {8, 4, 0}, so L1 starts with 4, 8.
  r = s.add(slice(h264::kSliceB, 3, 2, 2), 10, 0);
  CHECK(pocs(r.ref_pic_list[0]) == V{8, 4, 0});
  CHECK(pocs(r.ref_pic_list[1]) == V{4, 8, 0});
}

TEST_CASE("h264_dpb: lists shorter than num_ref_idx_active", "[index][h264][dpb][fast]") {
  Sim s(4);
  (void)s.add(idr(), 0);
  const DpbStepResult r = s.add(slice(h264::kSliceP, 1, 2), 2);  // three entries asked, one reference exists
  CHECK(decode_indices(r.ref_pic_list[0]) == V{0});
  CHECK(has_code(r, "SYN-H264-0104"));
}

TEST_CASE("h264_dpb: ref_pic_list_modification", "[index][h264][dpb][fast]") {
  Sim s(4);
  (void)s.add(idr(), 0);
  for (uint32_t fn = 1; fn <= 3; ++fn) {
    (void)s.add(slice(h264::kSliceP, fn), static_cast<int32_t>(2 * fn));
  }
  // Initial L0 of frame_num 4: PicNum 3, 2, 1, 0. idc 0 with abs_diff_pic_num_minus1 2: picNumL0NoWrap = 4 - 3 = 1,
  // inserted first, its later copy removed: 1, 3, 2, 0. idc 1 with 0: predictor 1 + 1 = 2 at index 1: 1, 2, 3, 0.
  h264::SliceHeader sh = slice(h264::kSliceP, 4, 3);
  add_modification(sh, 0, 0, 2);
  add_modification(sh, 0, 1, 0);
  DpbStepResult r = s.add(sh, 8, 0);
  CHECK(decode_indices(r.ref_pic_list[0]) == V{1, 2, 3, 0});
  CHECK(r.diagnostics.empty());
  // idc 0 below 0 wraps by MaxPicNum (eq. 8-35): 4 - 5 + 16 = 15 > CurrPicNum, so picNumL0 = 15 - 16 = -1: no such
  // picture, SYN-H264-0105, the slot stays "no reference picture".
  sh = slice(h264::kSliceP, 4, 1);
  add_modification(sh, 0, 0, 4);
  r = s.add(sh, 8, 0);
  CHECK(has_code(r, "SYN-H264-0105"));
  CHECK(decode_indices(r.ref_pic_list[0]) == V{3});
}

TEST_CASE("h264_dpb: long-term frames through MMCO 6, 3, 2, 4 and 5", "[index][h264][dpb][fast]") {
  Sim s(3);
  h264::SliceHeader sh = idr();
  sh.dec_ref_pic_marking.long_term_reference_flag = true;  // the IDR picture is long-term with index 0
  (void)s.add(sh, 0);
  (void)s.add(slice(h264::kSliceP, 1), 2);  // d1 short-term
  sh = slice(h264::kSliceP, 2, 1);
  add_mmco(sh, {6, 0, 0, 1, 0});  // d2 becomes long-term with LongTermFrameIdx 1
  (void)s.add(sh, 4);
  // §8.2.4.2.1: short-term by PicNum descending, then long-term by LongTermPicNum ascending.
  DpbStepResult r = s.add(slice(h264::kSliceP, 3, 2), 6, 0);
  CHECK(decode_indices(r.ref_pic_list[0]) == V{1, 0, 2});
  CHECK(r.ref_pic_list[0][1].long_term);
  // idc 2: the long-term picture with LongTermPicNum 1 (d2) first.
  sh = slice(h264::kSliceP, 3, 2);
  add_modification(sh, 0, 2, 1);
  r = s.add(sh, 6, 0);
  CHECK(decode_indices(r.ref_pic_list[0]) == V{2, 1, 0});
  // MMCO 3 turns d1 (picNumX = 3 - 1 - 1 = 1) long-term with index 0, which drops d0 (index 0 before); MMCO 2
  // drops the long-term frame with LongTermPicNum 1 (d2).
  sh = slice(h264::kSliceP, 3, 1);
  add_mmco(sh, {3, 1, 0, 0, 0});
  add_mmco(sh, {2, 0, 1, 0, 0});
  r = s.add(sh, 6);  // d5
  CHECK(r.dpb_after == V{1, 5});
  r = s.add(slice(h264::kSliceP, 4, 1), 8, 0);
  CHECK(decode_indices(r.ref_pic_list[0]) == V{5, 1});
  // MMCO 4 with max_long_term_frame_idx_plus1 0 drops every long-term frame.
  sh = slice(h264::kSliceP, 4);
  add_mmco(sh, {4, 0, 0, 0, 0});
  r = s.add(sh, 8);  // d7
  CHECK(r.dpb_after == V{5, 7});
  // MMCO 5 drops every reference picture; the current one stays, with FrameNum 0.
  sh = slice(h264::kSliceP, 5);
  add_mmco(sh, {5, 0, 0, 0, 0});
  r = s.add(sh, 10);  // d8
  CHECK(r.dpb_after == V{8});
  r = s.add(slice(h264::kSliceP, 1), 2);  // frame_num 1 follows FrameNum 0: no gap
  CHECK(r.diagnostics.empty());
  CHECK(decode_indices(r.ref_pic_list[0]) == V{8});
}

TEST_CASE("h264_dpb: MMCO 1 frees the window and too many references overflow", "[index][h264][dpb][fast]") {
  Sim s(2);
  (void)s.add(idr(), 0);
  (void)s.add(slice(h264::kSliceP, 1), 2);
  h264::SliceHeader sh = slice(h264::kSliceP, 2);
  add_mmco(sh, {1, 1, 0, 0, 0});  // picNumX = 2 - 2 = 0: d0
  DpbStepResult r = s.add(sh, 4);
  CHECK(r.dpb_after == V{1, 2});
  CHECK(r.diagnostics.empty());
  // Adaptive marking that frees nothing leaves three references for max_num_ref_frames 2.
  sh = slice(h264::kSliceP, 3);
  add_mmco(sh, {1, 5, 0, 0, 0});  // picNumX = 3 - 6 = -3: no such picture
  r = s.add(sh, 6);
  CHECK(has_code(r, "SYN-H264-0106"));
  CHECK(r.dpb_after == V{2, 3});
}

TEST_CASE("h264_dpb: frame_num gaps", "[index][h264][dpb][fast]") {
  SECTION("allowed: non-existing frames through the sliding window") {
    // max 2: frame_num 1 and 2 are inferred; inferring 2 drops the IDR picture. The P frame lists them (they hold
    // list positions) but does not declare them; its own window drops frame_num 1.
    Sim s(2, true);
    (void)s.add(idr(), 0);
    const DpbStepResult r = s.add(slice(h264::kSliceP, 3, 1), 6);
    CHECK(has_code(r, "SYN-H264-0102"));
    REQUIRE(r.ref_pic_list[0].size() == 2);
    CHECK(r.ref_pic_list[0][0].non_existing);
    CHECK(r.ref_pic_list[0][0].decode_index == -1);
    CHECK(r.declared_refs.empty());
    CHECK(r.dpb_after == V{1});
    CHECK(s.dpb().reference_frames() == 2);
  }
  SECTION("not allowed: a warning, nothing inferred") {
    Sim s(2);
    (void)s.add(idr(), 0);
    const DpbStepResult r = s.add(slice(h264::kSliceP, 3), 6);
    CHECK(has_code(r, "SYN-H264-0103"));
    CHECK(decode_indices(r.ref_pic_list[0]) == V{0});
  }
  SECTION("a gap of the whole frame_num range infers at most 16 frames") {
    Sim s(16, true);
    (void)s.add(idr(), 0);
    (void)s.add(slice(h264::kSliceP, 1), 2);
    const DpbStepResult r = s.add(slice(h264::kSliceI, 0), 4);  // 2..15 missing (14 frames)
    CHECK(has_code(r, "SYN-H264-0102"));
    // Sixteen references fill the DPB; relative to frame_num 0, frame_num 1 has the smallest FrameNumWrap (-15) and
    // the window drops it, while the IDR picture (frame_num 0, FrameNumWrap 0) stays.
    CHECK(s.dpb().reference_frames() == 16);
    CHECK(r.dpb_after == V{0, 2});
  }
}

TEST_CASE("h264_dpb: fields, alternating parity and the second field", "[index][h264][dpb][fast]") {
  // max_num_ref_frames 1. d0 IDR top field (POC 0), d1 its bottom field (POC 1): the second field of a frame whose
  // first field is a short-term reference does not run the sliding window (§8.2.5.3), so both stay.
  Sim s(1);
  (void)s.add(as_field(idr(), false), 0);
  DpbStepResult r = s.add(as_field(slice(h264::kSliceP, 0), true), 1, 1, 0);
  // §8.2.4.2.5 for the bottom field: no bottom field is a reference yet, so the list is the top field of d0.
  REQUIRE(r.ref_pic_list[0].size() == 1);
  CHECK(r.ref_pic_list[0][0].parity == Parity::Top);
  CHECK(r.declared_refs == V{0});
  CHECK(r.dpb_after == V{0});
  // d2 top field of frame_num 1 (two entries asked): same parity first: T0, then B1.
  r = s.add(as_field(slice(h264::kSliceP, 1, 1), false), 4);
  CHECK(pocs(r.ref_pic_list[0]) == V{0, 1});
  CHECK(r.declared_refs == V{0});
  CHECK(r.dpb_after == V{2});  // the window ran for the first field and dropped frame_num 0
}

TEST_CASE("h264_dpb: field lists alternate over FrameNumWrap", "[index][h264][dpb][fast]") {
  Sim s(4);
  (void)s.add(as_field(idr(), false), 0);                         // d0 T POC 0
  (void)s.add(as_field(slice(h264::kSliceP, 0), true), 1, 1, 0);  // d1 B POC 1
  (void)s.add(as_field(slice(h264::kSliceP, 1), false), 4);       // d2 T POC 4
  // d3 bottom field of frame_num 1: refFrameList0ShortTerm = {frame 1 (top only), frame 0}; bottom first: frame 1
  // has none, so B1 of frame 0, then T4 of frame 1, then T0 of frame 0.
  DpbStepResult r = s.add(as_field(slice(h264::kSliceP, 1, 2), true), 5, 1, 2);
  CHECK(pocs(r.ref_pic_list[0]) == V{1, 4, 0});
  CHECK(r.declared_refs == V{0, 2});
  CHECK(r.dpb_after == V{0, 2});
  // A B top field (POC 2, frame_num 2): refFrameList0ShortTerm by POC <= 2 descending then > 2 ascending: frame 0
  // (POC 0), frame 1 (POC 4); L0 = T0, B1, T4, B5; L1 = T4, B5, T0, B1.
  r = s.add(as_field(slice(h264::kSliceB, 2, 3, 3), false), 2, 0);
  CHECK(pocs(r.ref_pic_list[0]) == V{0, 1, 4, 5});
  CHECK(pocs(r.ref_pic_list[1]) == V{4, 5, 0, 1});
}

TEST_CASE("h264_dpb: long-term fields, LongTermPicNum = 2 * LongTermFrameIdx + 1 for the same parity",
          "[index][h264][dpb][fast]") {
  Sim s(2);
  h264::SliceHeader sh = idr();
  sh.dec_ref_pic_marking.long_term_reference_flag = true;  // d0: long-term frame, index 0
  (void)s.add(sh, 0);
  // d1 top field: MMCO 2 with long_term_pic_num 1 is the top field of d0 (same parity: 2 * 0 + 1).
  sh = as_field(slice(h264::kSliceP, 1), false);
  add_mmco(sh, {2, 0, 1, 0, 0});
  DpbStepResult r = s.add(sh, 4);
  CHECK(r.dpb_after == V{0, 1});  // the bottom field of d0 is still long-term
  // d2 bottom field, second field of d1: long_term_pic_num 1 is now the bottom field of d0.
  sh = as_field(slice(h264::kSliceP, 1), true);
  add_mmco(sh, {2, 0, 1, 0, 0});
  r = s.add(sh, 5, 1, 1);
  CHECK(r.dpb_after == V{1});
}

// ---- conformance streams ----------------------------------------------------------------------------------------

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Indexed {
  idx::BuilderCore core;
  uint32_t max_num_ref_frames = 0;  // the largest of the stream's SPSs
};

// The H.264 dispatch of the builder over an Annex B file, keeping the module-internal frame records.
std::unique_ptr<Indexed> index_file(const std::filesystem::path& path) {
  auto out = std::make_unique<Indexed>();
  const std::unique_ptr<idx::CodecDispatch> dispatch = idx::make_h264_dispatch();
  const std::vector<uint8_t> data = read_file(path);
  ovt::syntax::scan_annexb(
      std::span<const uint8_t>(data), 0, ovt::syntax::kNoContainerSample, [&](const ovt::syntax::ScannedUnit& u) {
        idx::UnitRec rec;
        rec.file_offset = u.file_offset;
        rec.length = u.bytes.size();
        rec.prefix_span = u.prefix_span;
        out->core.units.push_back(std::move(rec));
        if (!u.bytes.empty() && (u.bytes[0] & 0x1FU) == 7) {
          const ovt::Rbsp rbsp = ovt::rbsp_from_nal(u.bytes);
          ovt::BitReader br(rbsp.rbsp);
          ovt::syntax::SyntaxBuilder b(br);
          (void)h264::parse_nal_header(br, b);
          out->max_num_ref_frames = std::max(out->max_num_ref_frames, h264::parse_sps(br, b).max_num_ref_frames);
        }
        dispatch->on_unit(out->core, static_cast<uint32_t>(out->core.units.size() - 1), u.bytes);
      });
  dispatch->end_of_stream(out->core);
  out->core.finish_frame();
  return out;
}

std::string read_gz(const std::filesystem::path& path) {
  REQUIRE(std::filesystem::exists(path));
  const std::string cmd = "gzip -dc '" + path.string() + "'";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"),  // NOLINT(cert-env33-c): fixed command
                                             pclose);
  REQUIRE(pipe != nullptr);
  std::string text;
  std::array<char, 65536> chunk{};
  size_t n = 0;
  while ((n = std::fread(chunk.data(), 1, chunk.size(), pipe.get())) > 0) {
    text.append(chunk.data(), n);
  }
  return text;
}

// One line of a jm-reflist golden: "<i> <F|T|B> poc=<p> L0=<p,p,...> L1=<...>".
struct JmPicture {
  char structure = 'F';
  int64_t poc = 0;
  std::array<std::vector<int64_t>, 2> lists;
};

std::vector<int64_t> parse_list(std::string_view s) {
  std::vector<int64_t> out;
  std::string item;
  std::istringstream in{std::string(s)};
  while (std::getline(in, item, ',')) {
    out.push_back(std::stoll(item));
  }
  return out;
}

std::vector<JmPicture> parse_golden(const std::string& text) {
  std::vector<JmPicture> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string index;
    std::string structure;
    std::string poc;
    std::string l0;
    std::string l1;
    ls >> index >> structure >> poc >> l0 >> l1;
    REQUIRE(poc.rfind("poc=", 0) == 0);
    REQUIRE(l0.rfind("L0=", 0) == 0);
    REQUIRE(l1.rfind("L1=", 0) == 0);
    JmPicture p;
    p.structure = structure.at(0);
    p.poc = std::stoll(poc.substr(4));
    p.lists[0] = parse_list(std::string_view(l0).substr(3));
    p.lists[1] = parse_list(std::string_view(l1).substr(3));
    out.push_back(std::move(p));
  }
  return out;
}

char structure_char(ovt::fb::PictureStructure s) {
  return s == ovt::fb::PictureStructure::TopField ? 'T' : s == ovt::fb::PictureStructure::BottomField ? 'B' : 'F';
}

// One picture as "<structure> poc=<poc> L0=<pocs> L1=<pocs>".
std::string describe(char structure, int64_t poc, const std::array<std::vector<int64_t>, 2>& lists) {
  std::ostringstream msg;
  msg << structure << " poc=" << poc;
  for (size_t x = 0; x < 2; ++x) {
    msg << " L" << x << '=';
    for (const int64_t p : lists[x]) {
      msg << p << ',';
    }
  }
  return msg.str();
}

// h264_reflist_vs_jm for one stream: every picture's structure, POC and lists (as POCs) equal JM's, and
// declared_refs is the set of frame stores the lists name (L0 then L1). Prints the first five pictures that differ.
void check_reflist_vs_jm(std::string_view id) {
  const std::filesystem::path stream = ovt::test::corpus_or_skip(id);
  const std::vector<JmPicture> jm =
      parse_golden(read_gz(std::filesystem::path(OVT_GOLDEN_H264_DIR) / (std::string(id) + ".jm-reflist.txt.gz")));
  const std::unique_ptr<Indexed> ix = index_file(stream);
  const std::vector<idx::FrameRec>& frames = ix->core.frames;
  INFO(id);
  CHECK(frames.size() == jm.size());
  size_t differ = 0;
  for (size_t i = 0; i < frames.size() && i < jm.size(); ++i) {
    const idx::FrameRec& f = frames[i];
    std::array<std::vector<int64_t>, 2> ours;
    std::vector<int32_t> stores;
    for (size_t x = 0; x < 2; ++x) {
      for (const idx::RefSlotRec& r : f.ref_lists[x]) {
        ours[x].push_back(r.poc);
        if (r.decode_index >= 0 && std::ranges::find(stores, r.decode_index) == stores.end()) {
          stores.push_back(r.decode_index);
        }
      }
    }
    const std::string theirs = describe(jm[i].structure, jm[i].poc, jm[i].lists);
    const std::string mine = describe(structure_char(f.structure), f.poc, ours);
    if ((mine != theirs || stores != f.declared_refs) && ++differ <= 5) {
      UNSCOPED_INFO("picture " << i << ": JM " << theirs << " | OVT " << mine);
    }
  }
  CHECK(differ == 0);
}

// The fast-tagged JVT streams of tests/corpora/manifest.json; the slow ones are listed inside their [slow] case
// (tools/check_slow_labels.py attributes literals to the case that follows them).
constexpr std::array kFastStreams = {
    "jvt_BA1_Sony_D",         "jvt_BA_MW_D",      "jvt_CABA1_SVA_B", "jvt_CVWP1_TOSHIBA_E", "jvt_MR1_BT_A",
    "jvt_Hi422FR1_SONY_A",    "jvt_CI1_FT_B",     "jvt_NRF_MW_E",    "jvt_CVSE2_Sony_B",    "jvt_aud_mw_e",
    "jvt_MIDR_MW_D",          "jvt_CVSE3_Sony_H", "jvt_MR6_BT_B",    "jvt_MR9_BT_B",        "jvt_CVPCMNL1_SVA_C",
    "jvt_FRExt1_Panasonic_D", "jvt_HCAFF1_HHI_B"};

TEST_CASE("h264_reflist_vs_jm: fast JVT streams", "[index][h264][dpb][fast][needs-corpus]") {
  for (const char* id : kFastStreams) {
    DYNAMIC_SECTION(id) {
      check_reflist_vs_jm(id);
    }
  }
}

TEST_CASE("h264_reflist_vs_jm: slow JVT streams", "[index][h264][dpb][slow][needs-corpus]") {
  // The slow-tagged JVT streams of tests/corpora/manifest.json.
  static constexpr std::array kSlowStreams = {
      "jvt_BAMQ1_JVC_C",        "jvt_CABA3_TOSHIBA_E",  "jvt_CAMA1_Sony_C",
      "jvt_CAPAMA3_Sand_F",     "jvt_CAPM3_Sony_D",     "jvt_HPCV_BRCM_A",
      "jvt_ALPHACONFORMANCEG",  "jvt_sp1_bt_a",         "jvt_cvmp_mot_frm_L31_B",
      "jvt_Sharp_MP_Field_1_B", "jvt_CVFC1_Sony_C",     "jvt_FM1_BT_B",
      "jvt_LS_SVA_D",           "jvt_CVSEFDFT3_Sony_E", "jvt_CVPA1_TOSHIBA_B",
      "jvt_CVFI1_Sony_D",       "jvt_CAQP1_Sony_B",     "jvt_CABAST3_Sony_E",
      "jvt_HPCAFL_BRCM_C",      "jvt_MVCNV_1",          "jvt_SVCBST_1"};
  for (const char* id : kSlowStreams) {
    DYNAMIC_SECTION(id) {
      check_reflist_vs_jm(id);
    }
  }
}

size_t count_code(const idx::BuilderCore& core, std::string_view code) {
  return static_cast<size_t>(std::count_if(core.diagnostics.begin(), core.diagnostics.end(),
                                           [code](const idx::DiagRec& d) { return d.code == code; }));
}

TEST_CASE("h264_dpb: MR6_BT_B and MR9_BT_B stay within max_num_ref_frames", "[index][h264][dpb][fast][needs-corpus]") {
  for (const char* id : {"jvt_MR6_BT_B", "jvt_MR9_BT_B"}) {
    DYNAMIC_SECTION(id) {
      const std::unique_ptr<Indexed> ix = index_file(ovt::test::corpus_or_skip(id));
      REQUIRE(ix->max_num_ref_frames > 0);
      REQUIRE_FALSE(ix->core.frames.empty());
      for (const idx::FrameRec& f : ix->core.frames) {
        INFO("frame " << f.decode_index);
        CHECK(f.dpb_after.size() <= ix->max_num_ref_frames);
      }
      CHECK(count_code(ix->core, "SYN-H264-0105") == 0);
      CHECK(count_code(ix->core, "SYN-H264-0106") == 0);
    }
  }
}

TEST_CASE("h264_dpb: MR1_BT_A infers non-existing frames", "[index][h264][dpb][fast][needs-corpus]") {
  const std::unique_ptr<Indexed> ix = index_file(ovt::test::corpus_or_skip("jvt_MR1_BT_A"));
  CHECK(count_code(ix->core, "SYN-H264-0102") > 0);
}

}  // namespace
