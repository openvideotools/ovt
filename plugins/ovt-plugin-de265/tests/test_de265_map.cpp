// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Tests of the ovt-plugin-de265 block mapping (WI-2.164, chapter 07 §7.6.2 mapping table) on hand-made libde265
// metadata.
//  - map_grid.cpp: pred, qp, intra modes (PCM included), MVs and RefSlots with per-slice reference lists, partition
//    ids, SAO type, deblocking bS, the QP histogram and undecoded cells.
//  - map_blocks(): the CTU/CU/PU/TU tree (AMP and NxN rectangles, transform tree) with its extension tables, the
//    transform grid fields, MotionVectors, tile scan, slice segments and HevcFilterParams, and the chapter 06
//    validator on every result.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <regex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "map_blocks.hpp"
#include "map_grid.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
using ovt::de265::BlockData;
using ovt::de265::CbInfo;
using ovt::de265::PbInfo;
using ovt::de265::PictureFormat;
using ovt::de265::SliceHeader;
using Mode = ovt::de265::De265PredMode;

// ---- hand-made libde265 metadata -----------------------------------------------------------------------------------

struct Pic {
  BlockData d;
  PictureFormat fmt;

  Pic(int32_t w, int32_t h, int32_t ctb_log2, int32_t min_cb_log2) {
    fmt.coded_w = w;
    fmt.coded_h = h;
    d.valid = true;
    d.ctb_log2 = ctb_log2;
    d.min_cb_log2 = min_cb_log2;
    d.ctbs_w = (w + (1 << ctb_log2) - 1) >> ctb_log2;
    d.ctbs_h = (h + (1 << ctb_log2) - 1) >> ctb_log2;
    d.cbs_w = (w + (1 << min_cb_log2) - 1) >> min_cb_log2;
    d.cbs_h = (h + (1 << min_cb_log2) - 1) >> min_cb_log2;
    d.cells_w = static_cast<uint32_t>((w + 3) / 4);
    d.cells_h = static_cast<uint32_t>((h + 3) / 4);
    const std::size_t cells = std::size_t{d.cells_w} * d.cells_h;
    const auto ctbs = static_cast<std::size_t>(d.ctbs_w) * static_cast<std::size_t>(d.ctbs_h);
    d.cb.resize(static_cast<std::size_t>(d.cbs_w) * static_cast<std::size_t>(d.cbs_h));
    d.pb.resize(cells);
    d.intra_y.assign(cells, 255);
    d.intra_c.assign(cells, 255);
    d.tu.resize(cells);
    d.deblk.resize(cells);
    d.bs_v.resize(cells);
    d.ctb_slice.resize(ctbs);
    d.ctb_addr_ts.resize(ctbs);
    d.ctb_tile.resize(ctbs);
    d.sao.resize(ctbs);
    for (std::size_t i = 0; i < ctbs; ++i) {
      d.ctb_addr_ts[i] = static_cast<int32_t>(i);
    }
    d.slices.emplace_back();
  }

  [[nodiscard]] std::size_t cell(int32_t x, int32_t y) const {
    return static_cast<std::size_t>(y / 4) * d.cells_w + static_cast<std::size_t>(x / 4);
  }

  // Fills every 4x4 of the luma rect (x, y, w, h) through @p fn(cell index).
  template <typename Fn>
  void each(int32_t x, int32_t y, int32_t w, int32_t h, Fn fn) {
    for (int32_t yy = y; yy < y + h; yy += 4) {
      for (int32_t xx = x; xx < x + w; xx += 4) {
        fn(cell(xx, yy));
      }
    }
  }

  // A CU of 1 << log2 samples at (x, y); libde265 keeps its CB info at every MinCb unit it covers.
  void cu(int32_t x, int32_t y, int32_t log2, ovt::de265::De265PredMode mode, uint8_t part = 0, int16_t qp = 30) {
    const int32_t n = 1 << log2;
    const int32_t m = 1 << d.min_cb_log2;
    for (int32_t yy = y; yy < y + n; yy += m) {
      for (int32_t xx = x; xx < x + n; xx += m) {
        CbInfo& c = d.cb[cb_index(xx / m, yy / m)];
        c.log2CbSize = static_cast<uint8_t>(log2);
        c.PartMode = part;
        c.ctDepth = static_cast<uint8_t>(d.ctb_log2 - log2);
        c.PredMode = static_cast<uint8_t>(mode);
        c.QP_Y = qp;
      }
    }
  }
  [[nodiscard]] std::size_t cb_index(int32_t cx, int32_t cy) const {
    return static_cast<std::size_t>(cy) * static_cast<std::size_t>(d.cbs_w) + static_cast<std::size_t>(cx);
  }
  void cb_flag(int32_t x, int32_t y, int32_t log2, uint8_t CbInfo::* field) {
    const int32_t n = 1 << log2;
    const int32_t m = 1 << d.min_cb_log2;
    for (int32_t yy = y; yy < y + n; yy += m) {
      for (int32_t xx = x; xx < x + n; xx += m) {
        d.cb[cb_index(xx / m, yy / m)].*field = 1;
      }
    }
  }
  void intra(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t mode_y, uint8_t mode_c) {
    each(x, y, w, h, [&](std::size_t i) {
      d.intra_y[i] = mode_y;
      d.intra_c[i] = mode_c;
    });
  }
  void motion(int32_t x, int32_t y, int32_t w, int32_t h, int list, int8_t ref, int16_t mvx, int16_t mvy, int32_t poc) {
    each(x, y, w, h, [&](std::size_t i) {
      PbInfo& p = d.pb[i];
      const auto l = static_cast<std::size_t>(list);
      p.predFlag.at(l) = 1;
      p.refIdx.at(l) = ref;
      p.mv_x.at(l) = mvx;
      p.mv_y.at(l) = mvy;
      p.refPOC.at(l) = poc;
    });
  }
  void split_tu(int32_t x, int32_t y, int depth) { d.tu[cell(x, y)] |= static_cast<uint8_t>(1U << depth); }
  void nonzero(int32_t x, int32_t y, int32_t log2) {
    each(x, y, 1 << log2, 1 << log2, [&](std::size_t i) { d.tu[i] |= ovt::de265::kTuNonzeroCoeff; });
  }
};

SliceHeader b_slice() {
  SliceHeader sh;
  sh.slice_type = 0;
  sh.SliceQPY = 30;
  sh.slice_qp_delta = 4;
  sh.num_ref_idx_active = {2, 1};
  sh.RefPicList_POC[0][0] = 8;
  sh.RefPicList_POC[0][1] = 4;
  sh.RefPicList_POC[1][0] = 16;
  sh.RefPicList_LongTerm[0][1] = 1;
  sh.MaxNumMergeCand = 5;
  sh.slice_sao_luma_flag = 1;
  sh.slice_sao_chroma_flag = 1;
  sh.slice_beta_offset_div2 = -2;
  sh.slice_tc_offset_div2 = 3;
  return sh;
}

// The serialised grid, reference slots and QP histogram of a picture, the way the mapping produces them.
struct Grid {
  std::unique_ptr<flatbuffers::FlatBufferBuilder> fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(4096);
  std::vector<ovt::de265::RefTable::Slot> slots;
  std::vector<uint32_t> qp_hist;
  [[nodiscard]] const fb::CellGrid& grid() const {
    return *flatbuffers::GetRoot<fb::CellGrid>(fbb->GetBufferPointer());
  }
};

Grid map(const Pic& p) {
  Grid out;
  ovt::de265::GridCells g(std::size_t{p.d.cells_w} * p.d.cells_h);
  ovt::de265::RefTable refs(p.d.slices);
  ovt::de265::fill_cells(g, p.d, p.fmt, refs);
  out.slots = refs.slots();
  {
    flatbuffers::FlatBufferBuilder h;
    h.Finish(ovt::de265::build_qp_hist(h, g));
    const auto* v = flatbuffers::GetRoot<flatbuffers::Vector<uint32_t>>(h.GetBufferPointer());
    out.qp_hist.assign(v->begin(), v->end());
  }
  {
    flatbuffers::FlatBufferBuilder r;
    r.Finish(ovt::de265::build_refs(r, refs));
    const auto* v = flatbuffers::GetRoot<flatbuffers::Vector<flatbuffers::Offset<fb::RefSlot>>>(r.GetBufferPointer());
    REQUIRE(v->size() == out.slots.size());
    for (flatbuffers::uoffset_t k = 0; k < v->size(); ++k) {
      CHECK(v->Get(k)->index() == k);
      CHECK(v->Get(k)->name()->str() == out.slots[k].name);
      CHECK(v->Get(k)->frame()->decode_index() == -1);  // the host maps the POC to a decode index
    }
  }
  out.fbb->Finish(ovt::de265::build_grid(*out.fbb, p.d.cells_w, p.d.cells_h, g));
  flatbuffers::Verifier v(out.fbb->GetBufferPointer(), out.fbb->GetSize());
  REQUIRE(v.VerifyBuffer<fb::CellGrid>(nullptr));
  return out;
}

// A FrameAnalysis around map_blocks(), the way plugin.cpp assembles it.
struct Analysis {
  std::unique_ptr<flatbuffers::FlatBufferBuilder> fbb;
  std::vector<std::string> warnings;
  [[nodiscard]] const fb::FrameAnalysis& fa() const { return *fb::GetFrameAnalysis(fbb->GetBufferPointer()); }
  [[nodiscard]] std::span<const uint8_t> bytes() const { return {fbb->GetBufferPointer(), fbb->GetSize()}; }
};

Analysis analyse(const Pic& p) {
  Analysis a;
  a.fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(4096);
  auto& fbb = *a.fbb;
  const ovt::de265::MappedBlocks m = ovt::de265::map_blocks(fbb, p.d, p.fmt);
  a.warnings = m.warnings;
  const auto filter_type =
      m.filter_params.IsNull() ? flatbuffers::Offset<flatbuffers::String>() : fbb.CreateString("HevcFilterParams");
  const fb::FrameId id(3, -1);
  const fb::Rect coded(0, 0, p.fmt.coded_w, p.fmt.coded_h);
  fb::FrameInfoBuilder info(fbb);
  info.add_id(&id);
  info.add_type(fb::FrameType::B);
  info.add_shown(true);
  info.add_coded_size(&coded);
  info.add_crop(&coded);
  info.add_chroma_format(p.fmt.chroma_format);
  info.add_bit_depth_luma(p.fmt.bit_depth_luma);
  info.add_bit_depth_chroma(p.fmt.bit_depth_luma);
  const auto info_off = info.Finish();
  const auto plugin_id = fbb.CreateString("de265");
  const auto version = fbb.CreateString("test");
  fb::FrameAnalysisBuilder b(fbb);
  b.add_schema_version(1);
  b.add_plugin_id(plugin_id);
  b.add_plugin_version(version);
  b.add_frame(info_off);
  b.add_grid(m.grid);
  b.add_blocks(m.blocks);
  b.add_mvs(m.mvs);
  b.add_refs(m.refs);
  b.add_partitions(m.partitions);
  b.add_filter_params(m.filter_params);
  b.add_filter_params_type(filter_type);
  b.add_qp_hist(m.qp_hist);
  fb::FinishFrameAnalysisBuffer(fbb, b.Finish());
  flatbuffers::Verifier v(fbb.GetBufferPointer(), fbb.GetSize());
  REQUIRE(fb::VerifyFrameAnalysisBuffer(v));
  return a;
}

// Findings a plugin buffer has until the host's post-processing (chapter 07 §7.5.3 A6) fills FrameInfo timing and
// the partitions' unit indices; every other finding fails.
bool expected_finding(const ovt::model::Violation& v) {
  static const std::regex kUnitIndex(R"(partitions\[[0-9]+\]\.unit_index)");
  return (v.rule == "V-002" && v.path == "frame.timebase_den") ||
         (v.rule == "V-017" && std::regex_match(v.path, kUnitIndex));
}

void check_valid(std::span<const uint8_t> buf) {
  for (const auto& v : ovt::model::validate_frame_analysis(buf, fb::Codec::H265)) {
    INFO(v.rule << " " << v.path << ": " << v.message);
    CHECK(expected_finding(v));
  }
}

template <typename T>
const T* ext(const fb::Block* b) {
  REQUIRE(b->ext() != nullptr);
  return flatbuffers::GetRoot<T>(b->ext()->data());
}

std::string rect(const fb::Block* b) {
  const fb::Rect& r = *b->rect();
  return std::to_string(r.x()) + "," + std::to_string(r.y()) + " " + std::to_string(r.w()) + "x" +
         std::to_string(r.h());
}

std::vector<const fb::Block*> children(const fb::FrameAnalysis& fa, const fb::Block* b) {
  std::vector<const fb::Block*> out;
  for (const uint32_t id : *b->children()) {
    out.push_back(fa.blocks()->Get(id));
  }
  return out;
}

// 64x48 picture, 32x32 CTBs: two slices (a B slice over CTBs 0..2, a P slice with other lists over CTB 3).
Pic sample_picture() {
  Pic p(64, 48, 5, 3);
  p.d.slices[0] = b_slice();
  SliceHeader p_slice;
  p_slice.slice_type = 1;
  p_slice.SliceAddrRs = 3;
  p_slice.SliceQPY = 33;
  p_slice.num_ref_idx_active = {1, 0};
  p_slice.RefPicList_POC[0][0] = 4;
  p_slice.MaxNumMergeCand = 3;
  p_slice.slice_deblocking_filter_disabled_flag = 1;
  p.d.slices.push_back(p_slice);
  p.d.ctb_slice[3] = 1;

  // CTB 0: one 32x32 intra CU; the transform tree splits once, and its first 16x16 again.
  p.cu(0, 0, 5, Mode::Intra);
  p.intra(0, 0, 32, 32, 26, 10);
  p.split_tu(0, 0, 0);
  p.split_tu(0, 0, 1);
  p.nonzero(0, 0, 3);
  p.nonzero(16, 16, 4);
  p.d.sao[0].SaoTypeIdx = {2, 1, 1};
  p.d.sao[0].SaoEoClass = {3, 1, 1};
  p.d.sao[0].SaoOffsetVal = {1, 0, 0, -1, 2, 2, 1, 0, 0, 0, 0, 0};
  // CTB 1: four 16x16 CUs. AMP 2NxnU (L0), skip (bi), PCM, nRx2N (L1).
  p.cu(32, 0, 4, Mode::Inter, 4);
  p.motion(32, 0, 16, 4, 0, 0, 5, -3, 8);
  p.motion(32, 4, 16, 12, 0, 1, 8, 2, 4);
  p.nonzero(32, 0, 4);
  p.cu(48, 0, 4, Mode::Skip);
  p.motion(48, 0, 16, 16, 0, 1, -1, -1, 4);
  p.motion(48, 0, 16, 16, 1, 0, 3, 7, 16);
  p.cu(32, 16, 4, Mode::Intra);
  p.cb_flag(32, 16, 4, &CbInfo::pcm_flag);
  p.cu(48, 16, 4, Mode::Inter, 7);
  p.motion(48, 16, 12, 16, 1, 0, -20, 4, 16);
  p.motion(60, 16, 4, 16, 1, 0, 6, 6, 16);
  p.d.sao[1].SaoTypeIdx = {1, 0, 0};
  p.d.sao[1].SaoBandPosition = {12, 0, 0};
  // CTB 2 (32x16 visible): a 16x16 intra CU and four 8x8 CUs, the first one intra NxN with 4x4 TUs and bypass.
  p.cu(0, 32, 4, Mode::Intra);
  p.intra(0, 32, 16, 16, 1, 1);
  p.cu(16, 32, 3, Mode::Intra, 3);
  p.cb_flag(16, 32, 3, &CbInfo::cu_transquant_bypass);
  p.intra(16, 32, 8, 8, 0, 0);
  p.intra(20, 36, 4, 4, 34, 0);
  p.split_tu(16, 32, 0);
  p.nonzero(20, 36, 2);
  for (const auto& [x, y] : {std::pair{24, 32}, std::pair{16, 40}, std::pair{24, 40}}) {
    p.cu(x, y, 3, Mode::Intra);
    p.intra(x, y, 8, 8, 2, 2);
    p.nonzero(x, y, 3);
  }
  // CTB 3 (other slice): two inter 16x16 CUs on L0[0] of the P slice.
  p.cu(32, 32, 4, Mode::Inter, 0, 35);
  p.motion(32, 32, 16, 16, 0, 0, 1, 1, 4);
  p.cu(48, 32, 4, Mode::Inter, 0, 36);
  p.motion(48, 32, 16, 16, 0, 0, 2, 2, 4);
  // Deblocking: a vertical TU edge at x = 16 and a horizontal edge at y = 16 (rows 4) and a stale byte on row 5.
  p.each(16, 0, 4, 32, [&](std::size_t i) { p.d.bs_v[i] = 2; });
  p.each(0, 16, 32, 4, [&](std::size_t i) { p.d.deblk[i] = (1U << 5U) | 2U; });
  p.each(0, 20, 32, 4, [&](std::size_t i) { p.d.deblk[i] = (1U << 5U) | 1U; });
  return p;
}

}  // namespace

TEST_CASE("de265 map_grid: CellGrid and RefSlots of a hand-made picture", "[fast]") {
  const Pic p = sample_picture();
  const Grid m = map(p);
  const fb::CellGrid& g = m.grid();
  REQUIRE(g.cells_w() == 16);
  REQUIRE(g.cells_h() == 12);
  const auto at = [&](int32_t x, int32_t y) { return static_cast<flatbuffers::uoffset_t>(p.cell(x, y)); };

  CHECK(g.pred()->Get(at(0, 0)) == 1);
  CHECK(g.intra_mode_y()->Get(at(0, 0)) == 26);
  CHECK(g.intra_mode_c()->Get(at(0, 0)) == 10);
  CHECK(g.qp()->Get(at(0, 0)) == 30);
  CHECK(g.filter_idx()->Get(at(0, 0)) == 2);
  CHECK(g.filter_idx()->Get(at(40, 0)) == 1);
  CHECK(g.filter_idx()->Get(at(40, 40)) == 0);  // the P slice has SAO off
  CHECK(g.partition_id()->Get(at(48, 40)) == 1);

  // refs: L0[0] L0[1] L1[0] of the B slice, then S1:L0[0] for the P slice's different list.
  REQUIRE(m.slots.size() == 4);
  CHECK(m.slots[1].name == "L0[1]");
  CHECK(m.slots[1].long_term);
  CHECK(m.slots[2].name == "L1[0]");
  CHECK(m.slots[3].name == "S1:L0[0]");
  CHECK(m.slots[0].usage_cells == 4);        // the 16x4 PU
  CHECK(m.slots[1].usage_cells == 12 + 16);  // the 16x12 PU and the skip CU
  CHECK(m.slots[2].usage_cells == 16 + 16);  // the skip CU (bi) and the nRx2N CU
  CHECK(m.slots[3].usage_cells == 32);

  // Motion: quarter samples x 4; a list the PU does not use has ref -1 and a zero vector.
  CHECK(g.pred()->Get(at(48, 0)) == 3);
  CHECK(g.mv0_x()->Get(at(32, 0)) == 20);
  CHECK(g.mv0_y()->Get(at(32, 0)) == -12);
  CHECK(g.ref0()->Get(at(32, 4)) == 1);
  CHECK(g.mv1_y()->Get(at(48, 0)) == 28);
  CHECK(g.ref1()->Get(at(48, 0)) == 2);
  CHECK(g.ref0()->Get(at(48, 16)) == -1);
  CHECK(g.mv0_x()->Get(at(48, 16)) == 0);
  CHECK(g.mv1_x()->Get(at(48, 16)) == -80);
  CHECK(g.ref0()->Get(at(32, 32)) == 3);
  CHECK(g.intra_mode_y()->Get(at(32, 0)) == -1);

  CHECK(g.pred()->Get(at(32, 16)) == 5);  // PCM
  CHECK(g.intra_mode_y()->Get(at(32, 16)) == 64);
  CHECK(g.intra_mode_c()->Get(at(32, 16)) == -1);
  CHECK(g.intra_mode_y()->Get(at(20, 36)) == 34);  // the NxN CU's second PU

  CHECK(g.deblock_bs_v()->Get(at(16, 8)) == 2);
  CHECK(g.deblock_bs_h()->Get(at(8, 16)) == 2);
  CHECK(g.deblock_bs_h()->Get(at(8, 20)) == 0);  // odd 4x4 row: the stock byte holds a vertical bS

  // block_id and the transform fields come from the block tree (map_blocks.cpp): defaults here.
  CHECK(g.tx_log2_w()->Get(at(0, 0)) == 255);
  CHECK(g.tx_type()->Get(at(0, 0)) == 255);

  // qp_hist bins qp + 128.
  REQUIRE(m.qp_hist.size() == 256);
  CHECK(m.qp_hist[30 + 128] == 16 * 12 - 32);
  CHECK(m.qp_hist[35 + 128] == 16);
}

TEST_CASE("de265 map_grid: a 4:0:0 picture has no chroma modes", "[fast]") {
  Pic p(32, 32, 5, 3);
  p.fmt.chroma_format = 0;
  p.cu(0, 0, 5, Mode::Intra);
  p.intra(0, 0, 32, 32, 1, 1);
  const Grid m = map(p);
  CHECK(m.grid().intra_mode_y()->Get(0) == 1);
  CHECK(m.grid().intra_mode_c()->Get(0) == -1);
  CHECK(m.slots.empty());  // an I slice has no lists
}

TEST_CASE("de265 map_grid: undecoded cells and corrupt reference indices", "[fast]") {
  Pic p(64, 32, 5, 3);
  p.d.slices[0] = b_slice();
  p.cu(32, 0, 5, Mode::Inter);
  p.motion(32, 0, 32, 32, 0, 7, 4, 4, 0);   // refIdx 7 is outside the slice's two-entry L0
  p.motion(32, 0, 32, 32, 1, -1, 4, 4, 0);  // refIdx -1 with predFlag set
  const Grid m = map(p);
  const fb::CellGrid& g = m.grid();
  CHECK(g.pred()->Get(0) == 0);  // CTB 0 has no coding unit: pred None, qp 0
  CHECK(g.qp()->Get(0) == 0);
  CHECK(g.partition_id()->Get(0) == 0);
  const auto right = static_cast<flatbuffers::uoffset_t>(p.cell(32, 0));
  CHECK(g.pred()->Get(right) == 2);
  CHECK(g.ref0()->Get(right) == -1);
  CHECK(g.ref1()->Get(right) == -1);
  CHECK(g.mv0_x()->Get(right) == 0);
  for (const auto& s : m.slots) {
    CHECK(s.usage_cells == 0);
  }
  CHECK(m.qp_hist[128] == 64);
}

TEST_CASE("de265 map_grid: RefSlots shared between slice segments and the slot limit", "[fast]") {
  std::vector<SliceHeader> slices = {b_slice(), b_slice(), b_slice()};
  slices[2].RefPicList_POC[1][0] = 24;  // a different L1
  const ovt::de265::RefTable refs(slices);
  REQUIRE(refs.slots().size() == 6);
  CHECK(refs.slot(1, 0, 1) == 1);  // segment 1 shares segment 0's slots
  CHECK(refs.slots()[3].name == "S2:L0[0]");
  CHECK(refs.slot(2, 1, 0) == 5);
  CHECK(refs.slot(2, 1, 1) == -1);  // beyond num_ref_idx_l1_active
  CHECK(refs.slot(3, 0, 0) == -1);  // no such segment
  CHECK(refs.slot(0, 2, 0) == -1);  // no such list

  // At most 127 slots (int8 grid indices): a segment whose lists do not fit gets none.
  std::vector<SliceHeader> many;
  for (int s = 0; s < 5; ++s) {
    SliceHeader sh = b_slice();
    sh.num_ref_idx_active = {15, 15};
    for (std::size_t i = 0; i < 15; ++i) {
      sh.RefPicList_POC[0].at(i) = s * 100 + static_cast<int32_t>(i);
    }
    many.push_back(sh);
  }
  const ovt::de265::RefTable big(many);
  CHECK(big.slots().size() == 120);
  CHECK(big.slot(3, 0, 0) == 90);  // four segments of 30 slots fit
  CHECK(big.slot(4, 0, 0) == -1);  // the fifth does not
  CHECK(big.slot(4, 1, 14) == -1);
}

TEST_CASE("de265 map_grid: quarter-sample vectors saturate at the int16 range", "[fast]") {
  CHECK(ovt::de265::mv16(3) == 12);
  CHECK(ovt::de265::mv16(-5) == -20);
  CHECK(ovt::de265::mv16(10000) == 32767);
  CHECK(ovt::de265::mv16(-10000) == -32768);
}

TEST_CASE("de265 map_blocks: block tree of a hand-made picture", "[fast]") {
  const Pic p = sample_picture();
  const Analysis a = analyse(p);
  check_valid(a.bytes());
  CHECK(a.warnings.empty());
  const fb::FrameAnalysis& fa = a.fa();
  const auto ctus = children(fa, fa.blocks()->Get(0));
  REQUIRE(ctus.size() == 4);
  CHECK(rect(ctus[2]) == "0,32 32x16");  // clipped to the picture
  CHECK(ext<fb::HevcCtu>(ctus[1])->CtbAddrInRs() == 1);
  CHECK(ext<fb::HevcCtu>(ctus[3])->slice_segment_idx() == 1);
  CHECK(ext<fb::HevcCtu>(ctus[2])->end_of_slice_segment_flag() == 1);
  CHECK(ext<fb::HevcCtu>(ctus[1])->end_of_slice_segment_flag() == 0);
  CHECK(ext<fb::HevcCtu>(ctus[0])->tile_idx() == 0xFFFF);

  // CTB 0: CU -> PU, TU(32) -> 4 TU(16), the first -> 4 TU(8).
  const auto cu0 = children(fa, ctus[0]);
  REQUIRE(cu0.size() == 1);
  CHECK(cu0[0]->kind() == fb::BlockKind::CU);
  CHECK(cu0[0]->level() == 2);
  CHECK(ext<fb::HevcCu>(cu0[0])->CtDepth() == 0);
  CHECK(ext<fb::HevcCu>(cu0[0])->log2CbSize() == 5);
  CHECK(ext<fb::HevcCu>(cu0[0])->CuPredMode() == fb::HevcPredMode::MODE_INTRA);
  CHECK(ext<fb::HevcCu>(cu0[0])->pred_mode_flag() == 1);
  CHECK(ext<fb::HevcCu>(cu0[0])->IntraPredModeC()->Get(0) == 10);
  const auto cu0_kids = children(fa, cu0[0]);
  REQUIRE(cu0_kids.size() == 2);
  CHECK(cu0_kids[0]->kind() == fb::BlockKind::PU);
  CHECK(ext<fb::HevcPu>(cu0_kids[0])->IntraPredModeY() == 26);
  const fb::Block* root_tu = cu0_kids[1];
  CHECK(root_tu->kind() == fb::BlockKind::TU);
  CHECK(ext<fb::HevcTu>(root_tu)->split_transform_flag() == 1);
  const auto tu16 = children(fa, root_tu);
  REQUIRE(tu16.size() == 4);
  CHECK(rect(tu16[3]) == "16,16 16x16");
  CHECK(ext<fb::HevcTu>(tu16[3])->cbf_luma() == 1);
  CHECK(ext<fb::HevcTu>(tu16[1])->cbf_luma() == 0);
  const auto tu8 = children(fa, tu16[0]);
  REQUIRE(tu8.size() == 4);
  CHECK(tu8[2]->level() == 5);
  CHECK(tu8[2]->partition_type() == 2);  // trafoDepth
  CHECK(ext<fb::HevcTu>(tu8[1])->blkIdx() == 1);

  // CTB 1: z-scan order, AMP and nRx2N prediction blocks.
  const auto cu1 = children(fa, ctus[1]);
  REQUIRE(cu1.size() == 4);
  CHECK(rect(cu1[1]) == "48,0 16x16");
  CHECK(rect(cu1[2]) == "32,16 16x16");
  const auto amp = children(fa, cu1[0]);
  REQUIRE(amp.size() == 3);  // 2 PUs + root TU
  CHECK(rect(amp[0]) == "32,0 16x4");
  CHECK(rect(amp[1]) == "32,4 16x12");
  CHECK(amp[1]->partition_type() == 1);
  CHECK(ext<fb::HevcPu>(amp[1])->mvL0_x() == 8);
  CHECK(ext<fb::HevcPu>(amp[1])->ref_poc_l0() == 4);
  CHECK(ext<fb::HevcCu>(cu1[0])->rqt_root_cbf() == 1);
  CHECK(cu1[0]->partition_type() == 4);
  const auto skip = children(fa, cu1[1]);
  REQUIRE(skip.size() == 1);  // no transform tree
  CHECK(skip[0]->merge_or_direct());
  CHECK(ext<fb::HevcPu>(skip[0])->inter_pred_idc() == fb::HevcInterPredIdc::PRED_BI);
  CHECK(ext<fb::HevcPu>(skip[0])->merge_flag() == 1);
  CHECK(ext<fb::HevcCu>(cu1[1])->cu_skip_flag() == 1);
  CHECK(ext<fb::HevcCu>(cu1[1])->pred_mode_flag() == 255);
  CHECK(children(fa, cu1[2]).size() == 1);  // PCM: one PU, no TU
  CHECK(cu1[2]->pred() == fb::PredKind::PCM);
  const auto nr = children(fa, cu1[3]);
  REQUIRE(nr.size() == 3);
  CHECK(rect(nr[0]) == "48,16 12x16");
  CHECK(rect(nr[1]) == "60,16 4x16");
  CHECK(ext<fb::HevcPu>(nr[1])->inter_pred_idc() == fb::HevcInterPredIdc::PRED_L1);

  // CTB 2: the 8x8 NxN CU has four 4x4 PUs and four 4x4 TUs.
  const auto cu2 = children(fa, ctus[2]);
  REQUIRE(cu2.size() == 5);
  const auto nxn = children(fa, cu2[1]);
  REQUIRE(nxn.size() == 5);
  CHECK(rect(nxn[3]) == "20,36 4x4");
  CHECK(ext<fb::HevcPu>(nxn[3])->IntraPredModeY() == 34);
  CHECK(ext<fb::HevcPu>(nxn[1])->IntraPredModeY() == 0);
  CHECK(children(fa, nxn[4]).size() == 4);
  CHECK(ext<fb::HevcCu>(cu2[1])->cu_transquant_bypass_flag() == 1);
}

TEST_CASE("de265 map_blocks: transform grid fields and MotionVectors", "[fast]") {
  const Pic p = sample_picture();
  const Analysis a = analyse(p);
  const fb::FrameAnalysis& fa = a.fa();
  const fb::CellGrid& g = *fa.grid();
  const auto at = [&](int32_t x, int32_t y) { return static_cast<flatbuffers::uoffset_t>(p.cell(x, y)); };
  CHECK(g.block_id()->Get(at(0, 0)) > 0);
  CHECK(g.tx_log2_w()->Get(at(4, 4)) == 3);
  CHECK(g.tx_log2_h()->Get(at(20, 20)) == 4);
  CHECK(g.cbf()->Get(at(4, 4)) == 1);
  CHECK(g.cbf()->Get(at(20, 4)) == 0);
  CHECK(g.tx_type()->Get(at(0, 0)) == 0);
  CHECK(g.tx_log2_w()->Get(at(48, 0)) == 255);  // skip: no transform
  CHECK(g.tx_type()->Get(at(48, 0)) == 255);
  CHECK(g.tx_log2_w()->Get(at(32, 16)) == 255);  // PCM
  CHECK(g.tx_log2_w()->Get(at(20, 36)) == 2);
  CHECK(g.tx_type()->Get(at(20, 36)) == 3);  // transquant bypass
  CHECK(g.cbf()->Get(at(20, 36)) == 1);
  CHECK(g.tx_type()->Get(at(0, 32)) == 0);
  CHECK(fa.refs()->size() == 4);
  CHECK(fa.qp_hist()->Get(35 + 128) == 16);

  // One MotionVector per PU and list, in block order; bi-prediction sets flags bit 0.
  REQUIRE(fa.mvs()->size() == 8);
  const fb::MotionVector* bi = fa.mvs()->Get(2);
  CHECK(bi->list() == 0);
  CHECK(bi->flags() == 1);
  CHECK(fa.mvs()->Get(3)->list() == 1);
  CHECK(fa.mvs()->Get(0)->predictor()->str() == "?");
  CHECK(fa.mvs()->Get(0)->mvd_x() == -32768);
  CHECK(fa.mvs()->Get(4)->rect()->w() == 12);
}

TEST_CASE("de265 map_blocks: slice partitions and HevcFilterParams", "[fast]") {
  const Pic p = sample_picture();
  const Analysis a = analyse(p);
  const fb::FrameAnalysis& fa = a.fa();
  REQUIRE(fa.partitions()->size() == 2);
  const fb::PartitionInfo& s0 = *fa.partitions()->Get(0);
  CHECK(s0.kind() == fb::PartitionKind::Slice);
  CHECK(s0.block_count() == 3);
  CHECK(s0.slice_type() == 0);
  CHECK(s0.qp() == 30);
  CHECK(s0.qp_delta() == 4);
  CHECK(s0.max_num_merge_cand() == 5);
  CHECK(s0.deblock_alpha_or_beta_offset_div2() == -2);
  CHECK(s0.rect()->h() == 48);
  const fb::PartitionInfo& s1 = *fa.partitions()->Get(1);
  CHECK(s1.first_block_addr() == 3);
  CHECK(s1.deblocking_disable() == 1);
  CHECK(s1.rect()->x() == 32);
  CHECK(s1.rect()->h() == 16);
  REQUIRE(fa.filter_params_type()->str() == "HevcFilterParams");
  const auto* fp = flatbuffers::GetRoot<fb::HevcFilterParams>(fa.filter_params()->data());
  CHECK(fp->slice_tc_offset_div2()->Get(0) == 3);
  CHECK(fp->slice_deblocking_filter_disabled_flag()->Get(1) == 1);
  CHECK(fp->slice_sao_luma_flag()->Get(1) == 0);
}

TEST_CASE("de265 map_blocks: CTUs in tile scan, tiles and a dependent slice segment", "[fast]") {
  // 64x64, 32x32 CTBs, two tile columns: tile scan visits CTBs 0, 2, 1, 3. Slice segment 1 (dependent) starts at
  // CTB 2 inside the slice that CTB 0 starts.
  Pic p(64, 64, 5, 3);
  p.d.ctb_addr_ts = {0, 2, 1, 3};
  p.d.ctb_tile = {0, 1, 0, 1};
  SliceHeader dep;
  dep.SliceAddrRs = 0;
  p.d.slices.push_back(dep);
  p.d.ctb_slice = {0, 1, 1, 1};
  for (const auto& [x, y] : {std::pair{0, 0}, std::pair{32, 0}, std::pair{0, 32}, std::pair{32, 32}}) {
    p.cu(x, y, 5, Mode::Intra);
    p.intra(x, y, 32, 32, 1, 1);
  }
  const Analysis a = analyse(p);
  check_valid(a.bytes());
  const fb::FrameAnalysis& fa = a.fa();
  const auto ctus = children(fa, fa.blocks()->Get(0));
  REQUIRE(ctus.size() == 4);
  CHECK(rect(ctus[1]) == "0,32 32x32");
  CHECK(rect(ctus[2]) == "32,0 32x32");
  CHECK(ext<fb::HevcCtu>(ctus[2])->CtbAddrInTs() == 2);
  CHECK(ext<fb::HevcCtu>(ctus[2])->tile_idx() == 3);  // partitions: 2 slice segments, then the tiles
  CHECK(ext<fb::HevcCtu>(ctus[0])->end_of_slice_segment_flag() == 1);
  REQUIRE(fa.partitions()->size() == 4);
  CHECK(fa.partitions()->Get(1)->kind() == fb::PartitionKind::SliceSegment);
  CHECK(fa.partitions()->Get(1)->parent() == 0);
  CHECK(fa.partitions()->Get(1)->first_block_addr() == 2);
  CHECK(fa.partitions()->Get(3)->kind() == fb::PartitionKind::Tile);
  CHECK(fa.partitions()->Get(3)->tile_col() == 1);
  CHECK(fa.partitions()->Get(3)->tile_row() == 0);
  CHECK(fa.partitions()->Get(3)->rect()->x() == 32);
  CHECK(fa.partitions()->Get(3)->rect()->h() == 64);
}

TEST_CASE("de265 map_blocks: a 64x64 CU without a transform tree", "[fast]") {
  Pic p(64, 64, 6, 3);
  p.d.slices[0].slice_type = 1;
  p.d.slices[0].num_ref_idx_active = {1, 0};
  p.cu(0, 0, 6, Mode::Inter);
  p.motion(0, 0, 64, 64, 0, 0, 0, 0, 0);
  const Analysis a = analyse(p);
  check_valid(a.bytes());
  const auto* const cu = children(a.fa(), children(a.fa(), a.fa().blocks()->Get(0))[0])[0];
  CHECK(children(a.fa(), cu).size() == 1);  // the PU only: rqt_root_cbf = 0
  CHECK(a.fa().grid()->tx_log2_w()->Get(0) == 255);
  CHECK(ext<fb::HevcCu>(cu)->rqt_root_cbf() == 255);
}

TEST_CASE("de265 map_blocks: undecoded CTBs and unusable metadata", "[fast]") {
  Pic p(64, 32, 5, 3);
  p.cu(0, 0, 5, Mode::Intra);
  const Analysis a = analyse(p);
  CHECK(a.fa().blocks()->Get(0)->children()->size() == 1);
  REQUIRE(a.warnings.size() == 1);
  CHECK(a.warnings[0] == "64 cells were not decoded (no coding unit)");

  Pic bad = p;
  bad.d.pb.pop_back();
  const Analysis b = analyse(bad);
  CHECK(b.fa().blocks() == nullptr);
  CHECK(b.fa().grid()->pred() == nullptr);
  CHECK(b.fa().grid()->cells_w() == 16);
  REQUIRE(b.warnings.size() == 1);

  Pic perm = p;
  perm.d.ctb_addr_ts = {0, 0};  // not a permutation
  CHECK(analyse(perm).fa().blocks() == nullptr);

  Pic none = p;
  none.d.valid = false;
  const Analysis c = analyse(none);
  CHECK(c.warnings.empty());
  CHECK(c.fa().grid()->block_id() == nullptr);
}
