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
//  - The plugin on the `h265-cu-golden` conformance streams and the MV streams of AT2, compared cell by cell with the
//    HM grid goldens tests/golden/h265/<id>.hm-grid.json.gz (hm_cu_dump, chapter 14 §14.4.4): pred, qp, intra modes,
//    MVs (AT2 `h265_mv_vs_hm`), reference indices, transform sizes and cbf_luma; and with the hm-cutree goldens
//    (tests/oracles/hm_cutree_dump.cpp): the CU tree of AMP_A (AT1 `h265_cu_tree_vs_hm`) and the SAO parameters per
//    CTB of SAO_A (AT3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <flatbuffers/flatbuffers.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "map_blocks.hpp"
#include "map_grid.hpp"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

namespace fb = ovt::fb;
using ovt::de265::BlockData;
using ovt::de265::CbInfo;
using ovt::de265::PbInfo;
using ovt::de265::PictureFormat;
using ovt::de265::SliceHeader;
using Mode = ovt::de265::De265PredMode;
using json = nlohmann::json;

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
  uint32_t mv_clamped_cells = 0;
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
  out.mv_clamped_cells = g.mv_clamped_cells;
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

TEST_CASE("de265 map_grid: quarter-sample vectors clamp to ±32767", "[fast]") {
  CHECK(ovt::de265::mv16(3) == 12);
  CHECK(ovt::de265::mv16(-5) == -20);
  CHECK(ovt::de265::mv16(10000) == 32767);
  CHECK(ovt::de265::mv16(-10000) == -32767);  // -32768 is the MVD sentinel (chapter 06 §6.2.3)
  CHECK(ovt::de265::mv16_clamps(8192));
  CHECK_FALSE(ovt::de265::mv16_clamps(8191));
  CHECK(ovt::de265::mv16_clamps(-8192));
  CHECK_FALSE(ovt::de265::mv16_clamps(-8191));

  // A 32x32 PB with an L0 vector of 9000 quarter samples: 64 clamped cells (MDL-GEN-0003), stored as 32767.
  Pic p(64, 32, 5, 3);
  p.d.slices[0] = b_slice();
  p.cu(0, 0, 5, Mode::Inter);
  p.motion(0, 0, 32, 32, 0, 0, 9000, -9000, 0);
  p.cu(32, 0, 5, Mode::Inter);
  p.motion(32, 0, 32, 32, 0, 0, 8191, 4, 0);
  const Grid m = map(p);
  CHECK(m.mv_clamped_cells == 64);
  CHECK(m.grid().mv0_x()->Get(0) == 32767);
  CHECK(m.grid().mv0_y()->Get(0) == -32767);
  CHECK(m.grid().mv0_x()->Get(static_cast<flatbuffers::uoffset_t>(p.cell(32, 0))) == 32764);
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

// ---- the plugin on conformance streams vs the HM goldens ------------------------------------------------------------

namespace {

const ovt_AnalysisDecoderApi& plugin_api() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_DE265_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("de265");
  REQUIRE(plugin != nullptr);
  return *plugin->api;
}

std::vector<std::vector<uint8_t>> split_annexb(const std::vector<uint8_t>& s) {
  std::vector<std::vector<uint8_t>> nals;
  std::size_t start = SIZE_MAX;
  const auto close = [&](std::size_t end) {
    while (end > start && s[end - 1] == 0) {
      --end;
    }
    if (end > start) {
      nals.emplace_back(s.begin() + static_cast<std::ptrdiff_t>(start), s.begin() + static_cast<std::ptrdiff_t>(end));
    }
  };
  for (std::size_t i = 0; i + 3 <= s.size();) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != SIZE_MAX) {
        close(i);
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX) {
    close(s.size());
  }
  return nals;
}

// Every FrameAnalysis of the first @p max_frames pictures (decode order) of @p stream, keyed by decode index, each
// checked by the validator.
std::map<int32_t, std::vector<uint8_t>> analyse_stream(const std::filesystem::path& stream, int32_t max_frames) {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  ovt_AnalysisDecoder* dec = nullptr;
  REQUIRE(api.create(nullptr, 0, 0, &dec) == OVT_OK);
  std::ifstream in(stream, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::map<int32_t, std::vector<uint8_t>> out;
  const auto pull = [&] {
    ovt_Buffer buf{nullptr, 0};
    while (api.pull(dec, 0, &buf) == OVT_OK && buf.data != nullptr) {
      const std::span<const uint8_t> fa(buf.data, buf.size);
      const fb::FrameAnalysis& f = *fb::GetFrameAnalysis(buf.data);
      const int32_t index = f.frame()->id()->decode_index();
      if (index < max_frames && f.grid() != nullptr) {
        check_valid(fa);
        out.emplace(index, std::vector<uint8_t>(fa.begin(), fa.end()));
      }
      api.release(dec, buf);
    }
  };
  int32_t decode_index = -1;
  for (const std::vector<uint8_t>& nal : split_annexb(bytes)) {
    const int type = (nal[0] >> 1) & 0x3F;
    const bool first_slice = type <= 31 && nal.size() > 2 && (nal[2] & 0x80) != 0;
    decode_index += first_slice ? 1 : 0;
    (void)api.push(dec, nal.data(), nal.size(), type <= 31 ? decode_index : -1, decode_index);
    pull();
  }
  (void)api.flush(dec);
  pull();
  api.destroy(dec);
  return out;
}

std::string read_pipe(const std::string& cmd) {
  FILE* p = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c): gzip
  REQUIRE(p != nullptr);
  std::string text;
  std::array<char, 65536> buf{};
  std::size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
    text.append(buf.data(), n);
  }
  REQUIRE(pclose(p) == 0);
  return text;
}

std::optional<json> hm_golden(std::string_view id) {
  const std::filesystem::path file =
      std::filesystem::path(OVT_HEVC_GOLDEN_DIR) / (std::string(id) + ".hm-grid.json.gz");
  if (!std::filesystem::exists(file)) {
    return std::nullopt;
  }
  return std::optional<json>(std::in_place, json::parse(read_pipe("gzip -dc \"" + file.string() + "\"")));
}

// refIdx of a refs[] entry: "L0[i]", "L1[i]" or "S<s>:L0[i]".
int ref_idx_of(const fb::FrameAnalysis& fa, int8_t slot) {
  if (slot < 0) {
    return -1;
  }
  const std::string name = fa.refs()->Get(static_cast<flatbuffers::uoffset_t>(slot))->name()->str();
  const auto open = name.rfind('[');
  return std::stoi(name.substr(open + 1));
}

/// HM's DM_CHROMA_IDX: getIntraDir(CHANNEL_TYPE_CHROMA) for a CU whose chroma mode follows luma.
constexpr int kHmDmChroma = 36;

/// A golden vector (1/16 luma samples, unclamped) as CellGrid stores it (chapter 06 §6.2.3).
int clamp_mv(int hm) {
  return std::clamp(hm, -ovt::de265::kMvNeutralMax, ovt::de265::kMvNeutralMax);
}

/// Grid cells of a golden frame with a vector component outside ±32767: the MDL-GEN-0003 count.
std::size_t clamped_cells(const json& fields) {
  std::size_t n = 0;
  const std::size_t cells = fields.at("mv0_x").size();
  for (std::size_t i = 0; i < cells; ++i) {
    for (const char* key : {"mv0_x", "mv0_y", "mv1_x", "mv1_y"}) {
      const int v = fields.at(key)[i].get<int>();
      if (v != clamp_mv(v)) {
        ++n;
        break;
      }
    }
  }
  return n;
}

/// The MDL-GEN-0003 Diagnostics of a frame (at most one, chapter 06 §6.2.3).
std::vector<std::string> clamp_diagnostics(const fb::FrameAnalysis& fa) {
  std::vector<std::string> out;
  if (fa.diagnostics() != nullptr) {
    for (const fb::Diagnostic* d : *fa.diagnostics()) {
      if (d->code() != nullptr && d->code()->str() == "MDL-GEN-0003") {
        out.push_back(d->message() != nullptr ? d->message()->str() : std::string());
      }
    }
  }
  return out;
}

struct Mismatch {
  std::size_t count = 0;
  std::string first;
  void add(std::string_view field, uint32_t frame, std::size_t cell, int64_t ovt, int64_t hm) {
    if (count++ == 0) {
      first = std::string(field) + " frame " + std::to_string(frame) + " cell " + std::to_string(cell) + ": OVT " +
              std::to_string(ovt) + " HM " + std::to_string(hm);
    }
  }
};

// Compares one FrameAnalysis with one golden frame; returns per-field mismatch counts.
std::map<std::string, Mismatch> compare_frame(const fb::FrameAnalysis& fa, const json& frame, uint32_t index) {
  std::map<std::string, Mismatch> m;
  const fb::CellGrid& g = *fa.grid();
  const json& f = frame.at("fields");
  REQUIRE(frame.at("w4").get<uint32_t>() == g.cells_w());
  REQUIRE(frame.at("h4").get<uint32_t>() == g.cells_h());
  const std::size_t n = std::size_t{g.cells_w()} * g.cells_h();
  for (std::size_t i = 0; i < n; ++i) {
    const auto u = static_cast<flatbuffers::uoffset_t>(i);
    const int pred = g.pred()->Get(u);
    const int hm_pred = f.at("pred")[i].get<int>();
    const auto cmp = [&](std::string_view field, int64_t ovt, int64_t hm) {
      if (ovt != hm) {
        m[std::string(field)].add(field, index, i, ovt, hm);
      }
    };
    cmp("pred", pred, hm_pred);
    cmp("qp", g.qp()->Get(u), f.at("qp")[i].get<int>());
    if (hm_pred == 1) {  // OVT gives PCM cells mode 64; HM's golden has -1 for every non-intra kind
      cmp("intra_mode_y", g.intra_mode_y()->Get(u), f.at("intra_mode_y")[i].get<int>());
      // The golden holds HM's raw chroma direction: DM_CHROMA_IDX (36) for intra_chroma_pred_mode 4, which derives
      // IntraPredModeC from the luma mode at the CU origin (H.265 8.4.3, 4:2:0). CellGrid holds the derived mode.
      int hm_c = f.at("intra_mode_c")[i].get<int>();
      if (hm_c == kHmDmChroma) {
        const fb::Rect& r = *fa.blocks()->Get(g.block_id()->Get(u))->rect();
        const std::size_t origin = std::size_t(r.y() / 4) * g.cells_w() + std::size_t(r.x() / 4);
        hm_c = f.at("intra_mode_y")[origin].get<int>();
      }
      cmp("intra_mode_c", g.intra_mode_c()->Get(u), hm_c);
    }
    // The golden holds HM's vectors unclamped (×4); CellGrid clamps them to ±32767 (chapter 06 §6.2.3).
    cmp("mv0_x", g.mv0_x()->Get(u), clamp_mv(f.at("mv0_x")[i].get<int>()));
    cmp("mv0_y", g.mv0_y()->Get(u), clamp_mv(f.at("mv0_y")[i].get<int>()));
    cmp("mv1_x", g.mv1_x()->Get(u), clamp_mv(f.at("mv1_x")[i].get<int>()));
    cmp("mv1_y", g.mv1_y()->Get(u), clamp_mv(f.at("mv1_y")[i].get<int>()));
    cmp("ref0", ref_idx_of(fa, g.ref0()->Get(u)), f.at("ref0")[i].get<int>());
    cmp("ref1", ref_idx_of(fa, g.ref1()->Get(u)), f.at("ref1")[i].get<int>());
    // Transform fields exist only where a transform tree was read: not for skipped or PCM CUs, nor for a 64x64 CU
    // with rqt_root_cbf = 0, where HM reports the CU size.
    const int tx = g.tx_log2_w()->Get(u);
    const int hm_tx = f.at("tx_log2_w")[i].get<int>();
    if (tx != 255) {
      cmp("tx_log2", tx, hm_tx);
      cmp("cbf_luma", g.cbf()->Get(u) & 1, f.at("cbf")[i].get<int>() & 1);
    } else if (pred != 3 && pred != 5) {
      cmp("tx_log2 (no transform tree)", 6, hm_tx);
    }
  }
  return m;
}

// The `h265-cu-golden` streams (chapter 14 §14.2.4) that have an HM grid golden, and the MV stream of AT2. AMP_A,
// DBLK_A, TILES_B and TSKIP_A have none (tests/golden/h26x-index.json); AT1 uses AMP_A's hm-cutree golden instead.
constexpr std::array<std::string_view, 7> kGoldenStreams = {
    "jctvc_CAINIT_A_SHARP_4",      "jctvc_MERGE_B_TI_3", "jctvc_PMERGE_A_TI_3",       "jctvc_SAO_A_MediaTek_4",
    "jctvc_WPP_A_ericsson_MAIN_2", "jctvc_ipcm_A_NEC_3", "jctvc_MVEDGE_A_qualcomm_3",
};

void check_against_hm(std::string_view id) {
  const std::optional<json> golden = hm_golden(id);
  REQUIRE(golden.has_value());
  const std::filesystem::path stream = ovt::test::corpus_or_skip(id);
  const json& frames = golden->at("frames");
  const auto analyses = analyse_stream(stream, static_cast<int32_t>(frames.size()));
  REQUIRE(analyses.size() == frames.size());
  for (const json& frame : frames) {
    const auto index = frame.at("frame").get<uint32_t>();
    const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(analyses.at(static_cast<int32_t>(index)).data());
    if (frame.contains("poc")) {
      CHECK(fa.frame()->poc() == frame.at("poc").get<int64_t>());
    }
    for (const auto& [field, mismatch] : compare_frame(fa, frame, index)) {
      INFO(id << " " << mismatch.count << " cells differ; first: " << mismatch.first);
      CHECK(mismatch.count == 0);
    }
    // MDL-GEN-0003 once per frame with a clamp, naming the clamped cells.
    const std::size_t clamped = clamped_cells(frame.at("fields"));
    const std::vector<std::string> diags = clamp_diagnostics(fa);
    INFO(id << " frame " << index << ": " << clamped << " clamped cells");
    if (clamped == 0) {
      CHECK(diags.empty());
    } else {
      REQUIRE(diags.size() == 1);
      CHECK(diags.front().find("(" + std::to_string(clamped) + " cells)") != std::string::npos);
    }
  }
}

// The hm-cutree/1 golden of @p id (one record per CU and the resolved SAO parameters per CTB, after HM's loop
// filters; tests/oracles/hm_cutree_dump.cpp).
json hm_cutree(std::string_view id) {
  const std::filesystem::path file =
      std::filesystem::path(OVT_HEVC_GOLDEN_DIR) / (std::string(id) + ".hm-cutree.json.gz");
  REQUIRE(std::filesystem::exists(file));
  json doc = json::parse(read_pipe("gzip -dc \"" + file.string() + "\""));
  REQUIRE(doc.at("format") == "hm-cutree/1");
  REQUIRE(doc.at("stream") == id);
  return doc;
}

// The CU blocks of @p fa by luma position.
std::map<std::pair<int32_t, int32_t>, const fb::Block*> cus_by_position(const fb::FrameAnalysis& fa) {
  std::map<std::pair<int32_t, int32_t>, const fb::Block*> out;
  for (const fb::Block* ctu : children(fa, fa.blocks()->Get(0))) {
    for (const fb::Block* cu : children(fa, ctu)) {
      REQUIRE(cu->kind() == fb::BlockKind::CU);
      out.emplace(std::pair{cu->rect()->x(), cu->rect()->y()}, cu);
    }
  }
  return out;
}

// AT1: the OVT CU partition of every frame equals HM's CU by CU: position, size, ctDepth, part_mode, prediction
// mode, skip, PCM and transquant bypass.
void check_cu_tree(std::string_view id) {
  const json golden = hm_cutree(id);
  const std::filesystem::path stream = ovt::test::corpus_or_skip(id);
  const json& frames = golden.at("frames");
  const auto analyses = analyse_stream(stream, static_cast<int32_t>(frames.size()));
  REQUIRE(analyses.size() == frames.size());
  std::set<int> part_modes;
  for (const json& frame : frames) {
    const auto index = frame.at("frame").get<int32_t>();
    const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(analyses.at(index).data());
    INFO(id << " frame " << index);
    CHECK(fa.frame()->poc() == frame.at("poc").get<int64_t>());
    const auto cus = cus_by_position(fa);
    std::size_t hm_cus = 0;
    for (const json& ctu : frame.at("ctus")) {
      for (const json& h : ctu.at("cus")) {
        ++hm_cus;
        const int32_t x = h.at("x").get<int32_t>();
        const int32_t y = h.at("y").get<int32_t>();
        INFO("CU at " << x << "," << y);
        const auto it = cus.find({x, y});
        REQUIRE(it != cus.end());
        const fb::Block* cu = it->second;
        const auto* e = ext<fb::HevcCu>(cu);
        CHECK(cu->rect()->w() == 1 << h.at("log2_size").get<int>());
        CHECK(e->log2CbSize() == h.at("log2_size").get<int>());
        CHECK(e->CtDepth() == h.at("depth").get<int>());
        CHECK(cu->partition_type() == h.at("part_mode").get<int>());
        const bool intra = cu->pred() == fb::PredKind::Intra || cu->pred() == fb::PredKind::PCM;
        CHECK(intra == (h.at("pred_mode").get<int>() == 1));
        CHECK((cu->pred() == fb::PredKind::Skip) == (h.at("skip").get<int>() == 1));
        CHECK((cu->pred() == fb::PredKind::PCM) == (h.at("pcm").get<int>() == 1));
        CHECK(e->cu_transquant_bypass_flag() == h.at("bypass").get<int>());
        part_modes.insert(h.at("part_mode").get<int>());
      }
    }
    CHECK(cus.size() == hm_cus);
  }
  INFO("part modes seen: " << part_modes.size());
  CHECK(part_modes.size() > 1);
}

// The SAO parameters of one CTB against HM's three components.
void check_ctb_sao(const fb::HevcCtu& e, const json& sao) {
  for (flatbuffers::uoffset_t c = 0; c < 3; ++c) {
    const json& h = sao.at(c);
    const int type = h.at("type_idx").get<int>();
    const int ovt_type = e.SaoTypeIdx() != nullptr ? static_cast<int>(e.SaoTypeIdx()->Get(c)) : 0;
    CHECK(ovt_type == type);
    if (type == 0 || e.SaoTypeIdx() == nullptr) {
      continue;
    }
    if (type == 2) {
      CHECK(e.sao_eo_class()->Get(c) == h.at("eo_class").get<int>());
    } else {
      CHECK(e.sao_band_position()->Get(c) == h.at("band_position").get<int>());
    }
    for (flatbuffers::uoffset_t k = 0; k < 4; ++k) {
      CHECK(e.SaoOffsetVal()->Get(c * 4 + k) == h.at("offsets").at(k).get<int>());
    }
  }
}

// AT3: per CTB, filter_idx equals HM's SaoTypeIdx of luma, and HevcCtu carries HM's resolved SAO parameters of the
// three components (type; edge class or band position; SaoOffsetVal).
void check_sao(std::string_view id) {
  const json golden = hm_cutree(id);
  const std::filesystem::path stream = ovt::test::corpus_or_skip(id);
  const json& frames = golden.at("frames");
  const auto analyses = analyse_stream(stream, static_cast<int32_t>(frames.size()));
  REQUIRE(analyses.size() == frames.size());
  std::array<std::size_t, 3> seen{};
  for (const json& frame : frames) {
    const auto index = frame.at("frame").get<int32_t>();
    const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(analyses.at(index).data());
    const fb::CellGrid& g = *fa.grid();
    std::map<uint32_t, const fb::Block*> ctus;
    for (const fb::Block* ctu : children(fa, fa.blocks()->Get(0))) {
      ctus.emplace(ext<fb::HevcCtu>(ctu)->CtbAddrInRs(), ctu);
    }
    REQUIRE(ctus.size() == frame.at("ctus").size());
    for (const json& hc : frame.at("ctus")) {
      const auto rs = hc.at("rs").get<uint32_t>();
      INFO(id << " frame " << index << " CTB " << rs);
      REQUIRE(ctus.count(rs) == 1);
      const fb::Block* ctu = ctus.at(rs);
      const json& sao = hc.at("sao");
      check_ctb_sao(*ext<fb::HevcCtu>(ctu), sao);
      const int luma_type = sao.at(0).at("type_idx").get<int>();
      ++seen.at(static_cast<std::size_t>(luma_type));
      const fb::Rect& r = *ctu->rect();
      for (int32_t y = r.y(); y < r.y() + r.h(); y += 4) {
        for (int32_t x = r.x(); x < r.x() + r.w(); x += 4) {
          const auto i = static_cast<flatbuffers::uoffset_t>((y / 4) * static_cast<int32_t>(g.cells_w()) + x / 4);
          REQUIRE(g.filter_idx()->Get(i) == luma_type);
        }
      }
    }
  }
  CHECK(seen[1] > 0);  // band offset
  CHECK(seen[2] > 0);  // edge offset
}

}  // namespace

TEST_CASE("de265 plugin: h265_mv_vs_hm and the grid vectors on the h265-cu-golden streams",
          "[fast][third_party][needs-corpus][golden]") {
  const std::string_view id = GENERATE(from_range(kGoldenStreams));
  INFO(id);
  check_against_hm(id);
}

TEST_CASE("de265 plugin: h265_mv_vs_hm on MVCLIP_A_qualcomm_3", "[nightly][third_party][needs-corpus][golden]") {
  check_against_hm("jctvc_MVCLIP_A_qualcomm_3");
}

TEST_CASE("de265 plugin: h265_cu_tree_vs_hm on AMP_A_Samsung_7", "[fast][third_party][needs-corpus][golden]") {
  check_cu_tree("jctvc_AMP_A_Samsung_7");
}

TEST_CASE("de265 plugin: SAO per CTB vs HM on SAO_A_MediaTek_4", "[fast][third_party][needs-corpus][golden]") {
  check_sao("jctvc_SAO_A_MediaTek_4");
}
