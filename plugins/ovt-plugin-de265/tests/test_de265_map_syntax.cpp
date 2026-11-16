// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Tests of the libde265 patch-0002 mapping (WI-2.165, chapter 07 §7.6.2):
//  - map_syntax.cpp on hand-made syntax: MotionVector.predictor / mvd, the HevcCu fields from the CU's units
//    (rqt_root_cbf, cu_qp_delta, chroma QP offset, intra_chroma_pred_mode), the HevcTu vectors, CellGrid cbf /
//    tx_type, and the V-020 distribution of bits over cells.
//  - map_blocks() with syntax: the block tree, MotionVectors and CellGrid of a hand-made picture, checked by the
//    chapter 06 validator (V-019, V-020).
// The conformance-stream tests against HM (AT1) and the slice data sizes (AT2) are WI-2.165b.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "map_blocks.hpp"
#include "map_syntax.hpp"
#include "ovt_model_generated.h"
#include "test_de265_map_support.hpp"

namespace {

namespace fb = ovt::fb;
using de265_test::analyse;
using de265_test::ext;
using de265_test::Pic;
using Mode = ovt::de265::De265PredMode;
using ovt::de265::CellIndex;
using ovt::de265::CuShape;
using ovt::de265::PuKind;
using ovt::de265::PuSyntax;
using ovt::de265::SyntaxData;
using ovt::de265::TuSyntax;

constexpr uint8_t kNone = 255;

PuSyntax amvp_pu(uint8_t inter_pred_idc) {
  PuSyntax pu;
  pu.kind = static_cast<uint8_t>(PuKind::Inter);
  pu.merge_flag = 0;
  pu.inter_pred_idc = inter_pred_idc;
  pu.ref_idx = {1, 0};
  pu.mvp_flag = {1, 0};
  pu.mvd_x = {2, -5};
  pu.mvd_y = {-3, 7};
  return pu;
}

PuSyntax merge_pu(uint8_t idx) {
  PuSyntax pu;
  pu.kind = static_cast<uint8_t>(PuKind::Inter);
  pu.merge_flag = 1;
  pu.merge_idx = idx;
  return pu;
}

TuSyntax coded_tu(uint8_t log2, uint8_t cbf_y, uint8_t cbf_cb, uint8_t cbf_cr) {
  TuSyntax tu;
  tu.coded = 1;
  tu.log2TrafoSize = log2;
  tu.cbf = {cbf_y, cbf_cb, cbf_cr};
  return tu;
}

// A SyntaxData that fits @p p, with no unit coded.
void add_syntax(Pic& p) {
  SyntaxData& s = p.d.syn;
  const std::size_t cells = std::size_t{p.d.cells_w} * p.d.cells_h;
  s.has_syntax = true;
  s.pu.assign(cells, PuSyntax{});
  s.tu.assign(cells, TuSyntax{});
  s.cu_bits.assign(p.d.cb.size(), 0);
  s.ctb_bits.assign(static_cast<std::size_t>(p.d.ctbs_w) * static_cast<std::size_t>(p.d.ctbs_h), 0);
}

// Writes @p v to every MinCb unit of the CU of 1 << log2 at (x, y).
void cu_bits(Pic& p, int32_t x, int32_t y, int32_t log2, uint32_t v) {
  const int32_t m = 1 << p.d.min_cb_log2;
  for (int32_t yy = y; yy < y + (1 << log2); yy += m) {
    for (int32_t xx = x; xx < x + (1 << log2); xx += m) {
      p.d.syn.cu_bits[p.cb_index(xx / m, yy / m)] = v;
    }
  }
}

}  // namespace

TEST_CASE("de265 map_syntax: MotionVector predictor and mvd", "[fast]") {
  const PuSyntax bi = amvp_pu(2);
  CHECK(ovt::de265::predictor(bi, 0) == "amvp[1]");
  CHECK(ovt::de265::predictor(bi, 1) == "amvp[0]");
  CHECK(ovt::de265::mvd16(bi, 0) == std::array<int16_t, 2>{8, -12});  // quarter samples ×4
  CHECK(ovt::de265::mvd16(bi, 1) == std::array<int16_t, 2>{-20, 28});

  const PuSyntax l1 = amvp_pu(1);  // PRED_L1: list 0 is not used
  CHECK(ovt::de265::predictor(l1, 0) == "?");
  CHECK(ovt::de265::mvd16(l1, 0) == std::array<int16_t, 2>{-32768, -32768});
  CHECK(ovt::de265::predictor(l1, 1) == "amvp[0]");

  const PuSyntax m = merge_pu(3);
  CHECK(ovt::de265::predictor(m, 0) == "merge[3]");
  CHECK(ovt::de265::predictor(m, 1) == "merge[3]");
  CHECK(ovt::de265::mvd16(m, 1) == std::array<int16_t, 2>{-32768, -32768});

  PuSyntax big = amvp_pu(0);
  big.mvd_x[0] = -32768;  // ±2^15 quarter samples clamp to the neutral range (chapter 06 §6.2.3)
  big.mvd_y[0] = 32767;
  CHECK(ovt::de265::mvd16(big, 0) == std::array<int16_t, 2>{-32767, 32767});

  CHECK(ovt::de265::predictor(PuSyntax{}, 0) == "?");  // no syntax kept
  PuSyntax intra;
  intra.kind = static_cast<uint8_t>(PuKind::Intra);
  CHECK(ovt::de265::predictor(intra, 0) == "?");
}

TEST_CASE("de265 map_syntax: HevcCu fields from the units of a CU", "[fast]") {
  // A 16x16 picture: 4x4 cells.
  SyntaxData s;
  s.has_syntax = true;
  s.pu.assign(16, PuSyntax{});
  s.tu.assign(16, TuSyntax{});
  const CellIndex at{4};

  SECTION("inter 2Nx2N AMVP with a coded TU: rqt_root_cbf 1 and the TU's cu_qp_delta") {
    std::fill(s.pu.begin(), s.pu.end(), amvp_pu(0));
    TuSyntax tu = coded_tu(3, 1, 0, 0);
    tu.cu_qp_delta_abs = 3;
    tu.cu_qp_delta_sign_flag = 1;
    s.tu[at(8, 8)] = tu;
    const auto cs = ovt::de265::cu_syntax(s, at, CuShape{0, 0, 4, false, false, 0, 1});
    CHECK(cs.rqt_root_cbf == 1);
    CHECK(cs.cu_qp_delta_abs == 3);
    CHECK(cs.cu_qp_delta_sign_flag == 1);
    CHECK(cs.CuQpDeltaVal == -3);
    CHECK(cs.cu_chroma_qp_offset_flag == kNone);
    CHECK_FALSE(cs.all_merged);
    CHECK(cs.intra_chroma_pred_mode.empty());
  }
  SECTION("inter 2NxN without a transform tree: rqt_root_cbf 0") {
    std::fill(s.pu.begin(), s.pu.end(), merge_pu(0));
    std::fill(s.pu.begin() + 8, s.pu.end(), amvp_pu(0));
    const auto cs = ovt::de265::cu_syntax(s, at, CuShape{0, 0, 4, false, false, 1, 1});
    CHECK(cs.rqt_root_cbf == 0);
    CHECK(cs.cu_qp_delta_abs == kNone);
  }
  SECTION("2Nx2N merge and skip: rqt_root_cbf not signalled") {
    std::fill(s.pu.begin(), s.pu.end(), merge_pu(1));
    s.tu[0] = coded_tu(4, 0, 1, 0);
    auto cs = ovt::de265::cu_syntax(s, at, CuShape{0, 0, 4, false, false, 0, 1});
    CHECK(cs.rqt_root_cbf == kNone);
    CHECK(cs.all_merged);
    CHECK(cs.any_coded);
    cs = ovt::de265::cu_syntax(s, at, CuShape{0, 0, 4, false, true, 0, 1});
    CHECK(cs.rqt_root_cbf == kNone);
    // 2NxN with both PUs merged still signals rqt_root_cbf (H.265 7.3.8.5).
    cs = ovt::de265::cu_syntax(s, at, CuShape{0, 0, 4, false, false, 1, 1});
    CHECK(cs.rqt_root_cbf == 1);
  }
  SECTION("intra: chroma mode and chroma QP offset, no rqt_root_cbf") {
    PuSyntax pu;
    pu.kind = static_cast<uint8_t>(PuKind::Intra);
    pu.intra_chroma_pred_mode = 4;
    std::fill(s.pu.begin(), s.pu.end(), pu);
    TuSyntax tu = coded_tu(4, 1, 1, 1);
    tu.cu_chroma_qp_offset_flag = 1;
    tu.cu_chroma_qp_offset_idx = 2;
    s.tu[0] = tu;
    auto cs = ovt::de265::cu_syntax(s, at, CuShape{0, 0, 4, true, false, 0, 1});
    CHECK(cs.rqt_root_cbf == kNone);
    CHECK(cs.intra_chroma_pred_mode == std::vector<uint8_t>{4});
    CHECK(cs.cu_chroma_qp_offset_flag == 1);
    CHECK(cs.cu_chroma_qp_offset_idx == 2);
    // 4:4:4 PART_NxN: one chroma mode per PB.
    s.pu[at(8, 0)].intra_chroma_pred_mode = 1;
    s.pu[at(0, 8)].intra_chroma_pred_mode = 2;
    s.pu[at(8, 8)].intra_chroma_pred_mode = 3;
    cs = ovt::de265::cu_syntax(s, at, CuShape{0, 0, 4, true, false, 3, 3});
    CHECK(cs.intra_chroma_pred_mode == std::vector<uint8_t>{4, 1, 2, 3});
    // 4:0:0: none.
    cs = ovt::de265::cu_syntax(s, at, CuShape{0, 0, 4, true, false, 0, 0});
    CHECK(cs.intra_chroma_pred_mode.empty());
  }
}

TEST_CASE("de265 map_syntax: HevcTu vectors, CellGrid cbf and tx_type", "[fast]") {
  TuSyntax tu = coded_tu(2, 1, 3, 0);  // 4:2:2: both Cb blocks coded
  tu.transform_skip_flag = {1, 0, kNone};
  tu.last_sig_coeff_x = {3, 1, kNone};
  tu.last_sig_coeff_y = {0, 2, kNone};
  tu.nnz = {4, 9, 0};

  const auto vectors = [](const TuSyntax& t, uint8_t chroma_format, bool chroma_here) {
    flatbuffers::FlatBufferBuilder fbb;
    const ovt::de265::TuVectors v = ovt::de265::tu_vectors(fbb, t, chroma_format, chroma_here);
    fb::HevcTuBuilder b(fbb);
    ovt::de265::add_tu_syntax(b, t, v);
    fbb.Finish(b.Finish());
    return std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  };
  const auto as_vec = [](const auto* v) {
    using T = std::remove_cv_t<std::remove_pointer_t<decltype(v->data())>>;
    return v == nullptr ? std::vector<T>{} : std::vector<T>(v->begin(), v->end());
  };

  const std::vector<uint8_t> b422 = vectors(tu, 2, true);
  const auto* t = flatbuffers::GetRoot<fb::HevcTu>(b422.data());
  CHECK(t->cbf_luma() == 1);
  CHECK(as_vec(t->cbf_cb()) == std::vector<uint8_t>{1, 1});
  CHECK(as_vec(t->cbf_cr()) == std::vector<uint8_t>{0, 0});
  CHECK(as_vec(t->transform_skip_flag()) == std::vector<uint8_t>{1, 0, 255});
  CHECK(as_vec(t->last_sig_coeff_x()) == std::vector<uint8_t>{3, 1, 255});
  CHECK(as_vec(t->last_sig_coeff_y()) == std::vector<uint8_t>{0, 2, 255});
  CHECK(as_vec(t->num_nonzero()) == std::vector<uint16_t>{4, 9, 0});

  const std::vector<uint8_t> b420 = vectors(tu, 1, true);
  t = flatbuffers::GetRoot<fb::HevcTu>(b420.data());
  CHECK(as_vec(t->cbf_cb()) == std::vector<uint8_t>{1});
  CHECK(as_vec(t->cbf_cr()) == std::vector<uint8_t>{0});

  // blkIdx 0..2 of 4x4 luma TBs in 4:2:0 code no chroma; 4:0:0 has none.
  const std::vector<uint8_t> b420_luma = vectors(tu, 1, false);
  t = flatbuffers::GetRoot<fb::HevcTu>(b420_luma.data());
  CHECK(t->cbf_cb() == nullptr);
  CHECK(t->cbf_cr() == nullptr);
  const std::vector<uint8_t> b400 = vectors(tu, 0, true);
  t = flatbuffers::GetRoot<fb::HevcTu>(b400.data());
  CHECK(t->cbf_cb() == nullptr);
  CHECK(as_vec(t->transform_skip_flag()) == std::vector<uint8_t>{1, 255, 255});
  CHECK(as_vec(t->num_nonzero()) == std::vector<uint16_t>{4, 0, 0});

  CHECK(ovt::de265::grid_cbf(tu) == 0b011);
  CHECK(ovt::de265::grid_cbf(coded_tu(3, 0, 0, 1)) == 0b100);
  // tx_type: bypass 3 > transform skip 2 > DST-VII 1 (4x4 intra luma) > DCT-II 0.
  CHECK(ovt::de265::grid_tx_type(tu, true, true) == 3);
  CHECK(ovt::de265::grid_tx_type(tu, false, true) == 2);
  TuSyntax dst = coded_tu(2, 1, 0, 0);
  CHECK(ovt::de265::grid_tx_type(dst, false, true) == 1);
  CHECK(ovt::de265::grid_tx_type(dst, false, false) == 0);
  CHECK(ovt::de265::dst_used(dst, false, true));
  CHECK_FALSE(ovt::de265::dst_used(tu, false, true));                    // transform skip
  CHECK_FALSE(ovt::de265::dst_used(coded_tu(2, 0, 1, 1), false, true));  // no luma coefficients
}

TEST_CASE("de265 map_syntax: bits spread over the cells of a block (V-020)", "[fast]") {
  const CellIndex at{4};
  std::vector<uint32_t> grid(12);                     // 16x12 picture: 4x3 cells
  ovt::de265::spread_bits(grid, at, 3, 0, 0, 8, 10);  // 4 cells: 3, 3, 2, 2
  CHECK(grid == std::vector<uint32_t>{3, 3, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0});
  // A 16x16 block at (8, 0) covers 2x3 cells inside the grid; the cells below it do not count.
  ovt::de265::spread_bits(grid, at, 3, 8, 0, 16, 25);  // 6 cells: 5, 4, 4, 4, 4, 4
  CHECK(grid == std::vector<uint32_t>{3, 3, 5, 4, 2, 2, 4, 4, 0, 0, 4, 4});
  CHECK(std::accumulate(grid.begin(), grid.end(), uint32_t{0}) == 35);
}

TEST_CASE("de265 map_blocks: patch-0002 syntax and bit counts of a hand-made picture", "[fast]") {
  Pic p(32, 32, 5, 3);
  p.d.slices[0].slice_type = 0;
  p.d.slices[0].num_ref_idx_active = {2, 1};
  p.d.slices[0].RefPicList_POC[0] = {8, 4};
  p.d.slices[0].RefPicList_POC[1] = {16};
  add_syntax(p);
  SyntaxData& s = p.d.syn;
  const auto fill_pu = [&](int32_t x, int32_t y, int32_t w, int32_t h, const PuSyntax& pu) {
    p.each(x, y, w, h, [&](std::size_t i) { s.pu[i] = pu; });
  };
  const auto fill_tu = [&](int32_t x, int32_t y, int32_t log2, const TuSyntax& tu) {
    p.each(x, y, 1 << log2, 1 << log2, [&](std::size_t i) { s.tu[i] = tu; });
  };

  // (0, 0): 16x16 inter AMVP bi-prediction with one coded 16x16 TU.
  p.cu(0, 0, 4, Mode::Inter);
  p.motion(0, 0, 16, 16, 0, 1, 5, -3, 4);
  p.motion(0, 0, 16, 16, 1, 0, 3, 7, 16);
  p.nonzero(0, 0, 4);
  PuSyntax bi = amvp_pu(2);
  fill_pu(0, 0, 16, 16, bi);
  TuSyntax t0 = coded_tu(4, 1, 0, 1);
  t0.cu_qp_delta_abs = 2;
  t0.cu_qp_delta_sign_flag = 0;
  t0.nnz = {5, 0, 2};
  t0.last_sig_coeff_x = {3, kNone, 1};
  t0.last_sig_coeff_y = {2, kNone, 0};
  t0.transform_skip_flag = {0, kNone, 0};
  fill_tu(0, 0, 4, t0);
  cu_bits(p, 0, 0, 4, 800);
  // (16, 0): skip, merge candidate 2.
  p.cu(16, 0, 4, Mode::Skip);
  p.motion(16, 0, 16, 16, 0, 0, -1, -1, 8);
  fill_pu(16, 0, 16, 16, merge_pu(2));
  cu_bits(p, 16, 0, 4, 24);
  // (0, 16): 16x16 intra, transform tree split once into four coded 8x8 TUs.
  p.cu(0, 16, 4, Mode::Intra);
  p.intra(0, 16, 16, 16, 26, 26);
  p.split_tu(0, 16, 0);
  PuSyntax ipu;
  ipu.kind = static_cast<uint8_t>(PuKind::Intra);
  ipu.prev_intra_luma_pred_flag = 1;
  ipu.mpm_idx = 0;
  ipu.intra_chroma_pred_mode = 4;
  fill_pu(0, 16, 16, 16, ipu);
  for (const auto& [x, y] : {std::pair{0, 16}, std::pair{8, 16}, std::pair{0, 24}, std::pair{8, 24}}) {
    TuSyntax t = coded_tu(3, 1, 1, 0);
    t.trafoDepth = 1;
    fill_tu(x, y, 3, t);
    p.nonzero(x, y, 3);
  }
  cu_bits(p, 0, 16, 4, 1000);
  // (16, 16): an 8x8 intra NxN CU with 4x4 TUs (luma transform skip in blkIdx 0; chroma coded with blkIdx 3), and
  // three 8x8 intra CUs whose transform units were not kept (the patch-0001 fallback).
  p.cu(16, 16, 3, Mode::Intra, 3);
  p.intra(16, 16, 8, 8, 1, 1);
  p.split_tu(16, 16, 0);
  PuSyntax npu = ipu;
  npu.prev_intra_luma_pred_flag = 0;
  npu.mpm_idx = kNone;
  npu.rem_intra_luma_pred_mode = 7;
  npu.intra_chroma_pred_mode = 0;
  fill_pu(16, 16, 8, 8, npu);
  for (const auto& [x, y] : {std::pair{16, 16}, std::pair{20, 16}, std::pair{16, 20}, std::pair{20, 20}}) {
    TuSyntax t = coded_tu(2, 1, 1, 1);  // chroma cbf inferred from the 8x8 parent in blkIdx 0..2
    t.trafoDepth = 1;
    t.transform_skip_flag[0] = x == 16 && y == 16 ? 1 : 0;
    fill_tu(x, y, 2, t);
    p.nonzero(x, y, 2);
  }
  cu_bits(p, 16, 16, 3, 301);
  for (const auto& [x, y] : {std::pair{24, 16}, std::pair{16, 24}, std::pair{24, 24}}) {
    p.cu(x, y, 3, Mode::Intra);
    p.intra(x, y, 8, 8, 10, 10);
    p.nonzero(x, y, 3);
    cu_bits(p, x, y, 3, 64);
  }
  s.ctb_bits[0] = 800 + 24 + 1000 + 301 + (3 * 64) + 40;

  const de265_test::Analysis a = analyse(p);
  de265_test::check_valid(a.bytes());  // V-019 and V-020 included
  const fb::FrameAnalysis& fa = a.fa();
  const auto* blocks = fa.blocks();
  const fb::Block* root = blocks->Get(0);
  const fb::Block* ctu = blocks->Get(root->children()->Get(0));
  CHECK(root->bits_x8() == s.ctb_bits[0]);
  CHECK(ctu->bits_x8() == s.ctb_bits[0]);
  CHECK(ext<fb::HevcCtu>(ctu)->bit_count_x8() == s.ctb_bits[0]);
  REQUIRE(ctu->children()->size() == 7);
  const auto cu = [&](uint32_t k) { return blocks->Get(ctu->children()->Get(k)); };
  const auto kid = [&](const fb::Block* b, uint32_t k) { return blocks->Get(b->children()->Get(k)); };

  // Inter AMVP CU.
  const fb::Block* c0 = cu(0);
  CHECK(c0->bits_x8() == 800);
  CHECK_FALSE(c0->merge_or_direct());
  const auto* h0 = ext<fb::HevcCu>(c0);
  CHECK(h0->bit_count_x8() == 800);
  CHECK(h0->rqt_root_cbf() == 1);
  CHECK(h0->cu_qp_delta_abs() == 2);
  CHECK(h0->cu_qp_delta_sign_flag() == 0);
  CHECK(h0->CuQpDeltaVal() == 2);
  const fb::Block* pu0 = kid(c0, 0);
  const auto* hp0 = ext<fb::HevcPu>(pu0);
  CHECK(hp0->merge_flag() == 0);
  CHECK(hp0->merge_idx() == kNone);
  CHECK(hp0->inter_pred_idc() == fb::HevcInterPredIdc::PRED_BI);
  CHECK(hp0->ref_idx_l0() == 1);
  CHECK(hp0->ref_idx_l1() == 0);
  CHECK(hp0->mvd_l0_x() == 2);  // quarter samples as signalled
  CHECK(hp0->mvd_l0_y() == -3);
  CHECK(hp0->mvd_l1_x() == -5);
  CHECK(hp0->mvd_l1_y() == 7);
  CHECK(hp0->mvp_l0_flag() == 1);
  CHECK(hp0->mvp_l1_flag() == 0);
  const auto* ht0 = ext<fb::HevcTu>(kid(c0, 1));
  CHECK(ht0->cbf_luma() == 1);
  REQUIRE(ht0->cbf_cb() != nullptr);
  CHECK(ht0->cbf_cb()->Get(0) == 0);
  CHECK(ht0->cbf_cr()->Get(0) == 1);
  CHECK(ht0->num_nonzero()->Get(2) == 2);
  CHECK(ht0->last_sig_coeff_x()->Get(0) == 3);
  CHECK(ht0->dst_used() == 0);
  // MotionVectors: mvd ×4 and the AMVP predictor; the skipped CU's vector comes from merge[2].
  REQUIRE(fa.mvs()->size() == 3);
  CHECK(fa.mvs()->Get(0)->mvd_x() == 8);
  CHECK(fa.mvs()->Get(0)->mvd_y() == -12);
  CHECK(fa.mvs()->Get(0)->predictor()->str() == "amvp[1]");
  CHECK(fa.mvs()->Get(1)->mvd_x() == -20);
  CHECK(fa.mvs()->Get(1)->predictor()->str() == "amvp[0]");
  CHECK(fa.mvs()->Get(2)->mvd_x() == -32768);
  CHECK(fa.mvs()->Get(2)->predictor()->str() == "merge[2]");

  // Skipped CU.
  const fb::Block* c1 = cu(1);
  CHECK(c1->merge_or_direct());
  CHECK(c1->bits_x8() == 24);
  CHECK(ext<fb::HevcCu>(c1)->rqt_root_cbf() == kNone);
  const auto* hp1 = ext<fb::HevcPu>(kid(c1, 0));
  CHECK(hp1->merge_flag() == 1);
  CHECK(hp1->merge_idx() == 2);
  CHECK(hp1->mvd_l0_x() == -32768);
  CHECK(hp1->inter_pred_idc() == fb::HevcInterPredIdc::PRED_L0);  // derived from the merge candidate

  // Intra CU with four 8x8 TUs.
  const fb::Block* c2 = cu(2);
  const auto* h2 = ext<fb::HevcCu>(c2);
  CHECK(h2->rqt_root_cbf() == kNone);
  REQUIRE(h2->intra_chroma_pred_mode() != nullptr);
  CHECK(h2->intra_chroma_pred_mode()->Get(0) == 4);
  const auto* hp2 = ext<fb::HevcPu>(kid(c2, 0));
  CHECK(hp2->prev_intra_luma_pred_flag() == 1);
  CHECK(hp2->mpm_idx() == 0);
  CHECK(hp2->rem_intra_luma_pred_mode() == kNone);

  // NxN CU: transform skip and DST-VII in the grid, chroma vectors only in blkIdx 3.
  const fb::Block* c3 = cu(3);
  CHECK(ext<fb::HevcPu>(kid(c3, 0))->rem_intra_luma_pred_mode() == 7);
  const fb::Block* root_tu = kid(c3, 4);
  REQUIRE(root_tu->children()->size() == 4);
  const auto* blk0 = ext<fb::HevcTu>(kid(root_tu, 0));
  const auto* blk3 = ext<fb::HevcTu>(kid(root_tu, 3));
  CHECK(blk0->cbf_cb() == nullptr);
  CHECK(blk0->transform_skip_flag()->Get(0) == 1);
  CHECK(blk0->dst_used() == 0);
  REQUIRE(blk3->cbf_cb() != nullptr);
  CHECK(blk3->cbf_cb()->Get(0) == 1);
  CHECK(blk3->dst_used() == 1);

  // CellGrid: cbf bits 0..2, tx_type, bits_x8 distributed over each CU's cells (V-020).
  const fb::CellGrid& g = *fa.grid();
  const auto at = [&](int32_t x, int32_t y) { return static_cast<flatbuffers::uoffset_t>(p.cell(x, y)); };
  CHECK(g.cbf()->Get(at(0, 0)) == 0b101);
  CHECK(g.cbf()->Get(at(8, 24)) == 0b011);
  CHECK(g.cbf()->Get(at(16, 16)) == 0b111);
  CHECK(g.tx_type()->Get(at(16, 16)) == 2);
  CHECK(g.tx_type()->Get(at(20, 16)) == 1);
  CHECK(g.tx_type()->Get(at(0, 0)) == 0);
  CHECK(g.cbf()->Get(at(24, 16)) == 1);  // without kept syntax: cbf_luma only
  REQUIRE(g.bits_x8() != nullptr);
  CHECK(g.bits_x8()->Get(at(0, 0)) == 50);  // 800 / 16
  CHECK(g.bits_x8()->Get(at(16, 0)) == 2);  // 24 / 16: the first 8 cells get 2
  CHECK(g.bits_x8()->Get(at(16, 8)) == 1);
  CHECK(g.bits_x8()->Get(at(16, 16)) == 76);  // 301 / 4: 76, 75, 75, 75
  CHECK(g.bits_x8()->Get(at(20, 20)) == 75);
  const uint64_t total = std::accumulate(g.bits_x8()->begin(), g.bits_x8()->end(), uint64_t{0});
  CHECK(total == s.ctb_bits[0] - 40);
}

TEST_CASE("de265 map_blocks: syntax that does not fit the picture is ignored", "[fast]") {
  Pic p(16, 16, 4, 3);
  p.d.slices[0].slice_type = 1;
  p.d.slices[0].num_ref_idx_active = {1, 0};
  p.cu(0, 0, 4, Mode::Inter);
  p.motion(0, 0, 16, 16, 0, 0, 1, 1, 4);
  add_syntax(p);
  p.d.syn.pu.pop_back();
  const de265_test::Analysis a = analyse(p);
  de265_test::check_valid(a.bytes());
  CHECK(a.fa().blocks()->Get(1)->bits_x8() == 0);
  CHECK(a.fa().mvs()->Get(0)->predictor()->str() == "?");
}
