// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Tests of the insp_mi_data mapping of ovt-plugin-aom (WI-2.156, chapter 07 §7.6.1): partition-tree reconstruction
// on hand-made MI grids, map_blocks() on a hand-made frame, and the plugin's output for libaom test vectors against
// libaom's own `inspect` layers and FFmpeg's ref_frame_idx (data/inspect_reference.json, written by
// data/make_inspect_reference.py). Every FrameAnalysis is also run through the chapter 06 §6.9 validator.
// AT1: frame 0 of av1-1-b8-02-allintra, grid.intra_mode_y == inspect `mode`.
// AT2: av1-1-b8-05-mv, MVs (x2), ref0 and refs[] names and slots for every inter cell.
// AT3: syn_av1_tiles_cdef_lr (128x128 superblocks), every leaf covers exactly its MIs and the leaf sizes equal
//      inspect's blockSize layout, HORZ_A and VERT_4 included (part a, on the recorded layers); the plugin run of
//      the stream (recipe 13, not in the fast corpus set: skipped there) checks the superblock size and tiles.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <flatbuffers/flatbuffers.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "aom_test_util.inc"
#include "block_data.hpp"
#include "decoder.hpp"
#include "enums.h"
#include "fb_builder.hpp"
#include "map_mi.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt_model_generated.h"
#include "partition_reconstruct.hpp"
#include "test_support/corpus.h"

namespace fb = ovt::fb;
using ovt::aom::BlockData;
using ovt::aom::MiInfo;
using ovt::aom::PartNode;

namespace {

// ---- hand-made MI grids ----------------------------------------------------------------------------------------

struct Grid {
  int32_t rows;
  int32_t cols;
  std::vector<uint8_t> bsize;
  Grid(int32_t r, int32_t c) : rows(r), cols(c), bsize(static_cast<size_t>(r) * static_cast<size_t>(c), 255) {}
  // Stores block size @p b in every MI of the block at (r, c) that lies inside the grid, as libaom does.
  void paint(int32_t r, int32_t c, int b) {
    const auto s = ovt::aom::kBlockMiSize.at(static_cast<size_t>(b));
    for (int32_t y = r; y < std::min(r + s.h, rows); ++y) {
      for (int32_t x = c; x < std::min(c + s.w, cols); ++x) {
        bsize[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(x)] = static_cast<uint8_t>(b);
      }
    }
  }
};

constexpr int kB8x8 = 3;
constexpr int kB16x8 = 5;
constexpr int kB8x16 = 4;
constexpr int kB16x16 = 6;
constexpr int kB32x16 = 8;
constexpr int kB16x32 = 7;
constexpr int kB32x32 = 9;
constexpr int kB64x32 = 11;
constexpr int kB64x64 = 12;
constexpr int kB128x64 = 14;
constexpr int kB128x128 = 15;
constexpr int kB16x4 = 17;
constexpr int kB4x16 = 16;

std::vector<PartNode> rebuild(const Grid& g, int32_t r, int32_t c, int32_t sb) {
  std::vector<PartNode> nodes;
  const ovt::aom::BsizeGrid bg{g.bsize, g.rows, g.cols};
  REQUIRE(ovt::aom::reconstruct_superblock(bg, r, c, sb, &nodes) == 0);
  return nodes;
}

// Every MI of the grid inside the superblock is covered by exactly one leaf, and the leaf's size is the size the
// grid stores there (inspect's blockSize layout).
void check_cover(const std::vector<PartNode>& nodes, const Grid& g, int32_t r0, int32_t c0, int32_t sb,
                 bool sizes = true) {
  std::map<std::pair<int32_t, int32_t>, int> cover;
  for (const PartNode& n : nodes) {
    if (!n.children.empty()) {
      continue;
    }
    for (int32_t y = n.mi_row; y < std::min(n.mi_row + n.mi_h, g.rows); ++y) {
      for (int32_t x = n.mi_col; x < std::min(n.mi_col + n.mi_w, g.cols); ++x) {
        ++cover[{y, x}];
        if (sizes) {
          CHECK(g.bsize[static_cast<size_t>(y) * static_cast<size_t>(g.cols) + static_cast<size_t>(x)] == n.bsize);
        }
      }
    }
  }
  for (int32_t y = r0; y < std::min(r0 + sb, g.rows); ++y) {
    for (int32_t x = c0; x < std::min(c0 + sb, g.cols); ++x) {
      CHECK(cover[{y, x}] == 1);
    }
  }
  // Pre-order: parents before children, children listed in coding order.
  for (size_t i = 0; i < nodes.size(); ++i) {
    for (const uint32_t ch : nodes[i].children) {
      CHECK(nodes[ch].parent == static_cast<int32_t>(i));
      CHECK(ch > i);
    }
  }
}

const PartNode* node_at(const std::vector<PartNode>& nodes, int32_t r, int32_t c, int32_t size) {
  for (const PartNode& n : nodes) {
    if (n.mi_row == r && n.mi_col == c && n.mi_w == size && n.mi_h == size && !n.children.empty()) {
      return &n;
    }
  }
  return nullptr;
}

uint8_t part(ovt::aom::Partition p) {
  return static_cast<uint8_t>(p);
}

}  // namespace

TEST_CASE("aom partition: every partition type inside a 64x64 superblock", "[fast]") {
  using ovt::aom::Partition;
  Grid g(32, 32);
  // Superblock (0,0): HORZ_A, HORZ_B, VERT_A, VERT_B in its four 32x32 quadrants.
  g.paint(0, 0, kB16x16), g.paint(0, 4, kB16x16), g.paint(4, 0, kB32x16);     // HORZ_A
  g.paint(0, 8, kB32x16), g.paint(4, 8, kB16x16), g.paint(4, 12, kB16x16);    // HORZ_B
  g.paint(8, 0, kB16x16), g.paint(12, 0, kB16x16), g.paint(8, 4, kB16x32);    // VERT_A
  g.paint(8, 8, kB16x32), g.paint(8, 12, kB16x16), g.paint(12, 12, kB16x16);  // VERT_B
  // Superblock (0,16): HORZ_4 and VERT_4 16x16 nodes, NONE, HORZ, VERT and an 8x8 SPLIT.
  for (int k = 0; k < 4; ++k) {
    g.paint(k, 16, kB16x4);      // HORZ_4 at (0,16)
    g.paint(0, 20 + k, kB4x16);  // VERT_4 at (0,20)
  }
  g.paint(4, 16, kB16x16);                            // NONE
  g.paint(4, 20, kB16x8), g.paint(6, 20, kB16x8);     // HORZ
  g.paint(0, 24, kB32x32);                            // NONE 32x32
  g.paint(8, 16, kB32x16), g.paint(12, 16, kB32x16);  // HORZ 32x32
  g.paint(8, 24, kB8x16), g.paint(8, 26, kB8x16);     // VERT 16x16 at (8,24)
  for (int32_t y = 8; y < 12; y += 2) {               // SPLIT 16x16 at (8,28) into 8x8s
    g.paint(y, 28, kB8x8), g.paint(y, 30, kB8x8);
  }
  g.paint(12, 24, kB16x16), g.paint(12, 28, kB16x16);
  g.paint(16, 0, kB64x64), g.paint(16, 16, kB64x64);

  const auto a = rebuild(g, 0, 0, 16);
  check_cover(a, g, 0, 0, 16);
  CHECK(a[0].partition == part(Partition::Split));
  REQUIRE(node_at(a, 0, 0, 8) != nullptr);
  CHECK(node_at(a, 0, 0, 8)->partition == part(Partition::HorzA));
  CHECK(node_at(a, 0, 0, 8)->children.size() == 3);
  CHECK(node_at(a, 0, 8, 8)->partition == part(Partition::HorzB));
  CHECK(node_at(a, 8, 0, 8)->partition == part(Partition::VertA));
  CHECK(node_at(a, 8, 8, 8)->partition == part(Partition::VertB));

  const auto b = rebuild(g, 0, 16, 16);
  check_cover(b, g, 0, 16, 16);
  REQUIRE(node_at(b, 0, 16, 4) != nullptr);
  CHECK(node_at(b, 0, 16, 4)->partition == part(Partition::Horz4));
  CHECK(node_at(b, 0, 16, 4)->children.size() == 4);
  CHECK(node_at(b, 0, 20, 4)->partition == part(Partition::Vert4));
  CHECK(node_at(b, 4, 20, 4)->partition == part(Partition::Horz));
  CHECK(node_at(b, 8, 16, 8)->partition == part(Partition::Horz));
  CHECK(node_at(b, 8, 24, 4)->partition == part(Partition::Vert));
  CHECK(node_at(b, 8, 28, 4)->partition == part(Partition::Split));
  for (const PartNode& n : b) {
    CHECK_FALSE(n.ambiguous);
  }

  const auto leaf_sb = rebuild(g, 16, 0, 16);
  REQUIRE(leaf_sb.size() == 1);  // a 64x64 leaf superblock
  CHECK(leaf_sb[0].partition == 0);
  CHECK(leaf_sb[0].bsize == kB64x64);
}

TEST_CASE("aom partition: 128x128 superblocks and the frame edge", "[fast]") {
  using ovt::aom::Partition;
  Grid g(40, 40);                                     // the second superblock row and column are cut by the frame edge
  g.paint(0, 0, kB128x64), g.paint(16, 0, kB128x64);  // HORZ at the 128x128 root
  g.paint(0, 32, kB64x32), g.paint(8, 32, kB64x32);   // right edge: HORZ inside an 8-MI-wide superblock
  g.paint(16, 32, kB64x64);
  g.paint(32, 0, kB64x64), g.paint(32, 16, kB64x64);  // edge: forced SPLIT, bottom quadrants outside
  g.paint(32, 32, kB32x32);

  const auto a = rebuild(g, 0, 0, 32);
  check_cover(a, g, 0, 0, 32);
  CHECK(a[0].partition == part(Partition::Horz));
  CHECK(a[0].bsize == kB128x128);
  CHECK(a[0].children.size() == 2);

  const auto right = rebuild(g, 0, 32, 32);  // 8 MI columns inside the grid
  check_cover(right, g, 0, 32, 32);
  CHECK(right[0].partition == part(Partition::Split));
  REQUIRE(node_at(right, 0, 32, 16) != nullptr);
  CHECK(node_at(right, 0, 32, 16)->partition == part(Partition::Horz));

  const auto bottom = rebuild(g, 32, 0, 32);  // 8 MI rows inside the grid
  check_cover(bottom, g, 32, 0, 32);
  CHECK(bottom[0].partition == part(Partition::Split));
  CHECK(bottom[0].children.size() == 2);  // the bottom quadrants' origins are outside the grid

  const auto corner = rebuild(g, 32, 32, 32);
  check_cover(corner, g, 32, 32, 32);
  CHECK(corner[0].children.size() == 1);

  Grid strip(4, 16);  // bottom edge: HORZ whose second half lies outside the grid is not coded
  strip.paint(0, 0, kB64x32);
  const auto s = rebuild(strip, 0, 0, 16);
  check_cover(s, strip, 0, 0, 16);
  CHECK(s[0].partition == part(Partition::Horz));
  CHECK(s[0].children.size() == 1);
}

TEST_CASE("aom partition: an inconsistent grid is still covered exactly once", "[fast]") {
  Grid g(16, 16);
  g.paint(0, 0, kB64x64);
  g.paint(0, 0, kB128x128);  // larger than the superblock: fits no partition
  const auto n = rebuild(g, 0, 0, 16);
  check_cover(n, g, 0, 0, 16, /*sizes=*/false);
  CHECK(n[0].ambiguous);
  const Grid holes(16, 16);  // never painted: invalid sizes everywhere
  check_cover(rebuild(holes, 0, 0, 16), holes, 0, 0, 16, /*sizes=*/false);
}

// ---- map_blocks() on a hand-made frame ----------------------------------------------------------------------------

namespace {

// 66 x 34 luma samples: 17 x 9 cells over libaom's 18 x 10 MI grid (one padding column and row), one 64x64
// superblock column plus a second one that is two MIs wide.
BlockData synthetic_frame() {
  BlockData d;
  d.valid = true;
  d.mi_rows = 10;
  d.mi_cols = 18;
  d.mi.resize(180);
  d.sb_mi = 16;
  d.intra_frame = false;
  d.ref_frame_idx = {3, 1, 2, 0, 4, 5, 6};
  d.gm_type.at(4) = 2;  // ROTZOOM on GOLDEN
  d.tile_col_starts = {0, 18};
  d.tile_row_starts = {0, 10};
  d.cdef_bits = 2;
  d.cdef_strengths = {0, 4 * 3 + 1, 4 * 5 + 3, 4 * 5 + 3};  // index 2 and 3 equal: ambiguous
  d.cdef_uv_strengths = {0, 0, 0, 0};
  d.base_qindex = 100;
  auto block = [&](int32_t r, int32_t c, int bsize, const MiInfo& m) {
    const auto s = ovt::aom::kBlockMiSize.at(static_cast<size_t>(bsize));
    for (int32_t y = r; y < std::min(r + s.h, d.mi_rows); ++y) {
      for (int32_t x = c; x < std::min(c + s.w, d.mi_cols); ++x) {
        MiInfo& t = d.mi[static_cast<size_t>(y) * 18 + static_cast<size_t>(x)];
        t = m;
        t.bsize = static_cast<int16_t>(bsize);
      }
    }
  };
  MiInfo intra;  // V_PRED with CfL: signs 4 -> (U NEG, V POS); idx 0x21 -> |u| 3, |v| 2
  intra.mode = 1;
  intra.uv_mode = 13;
  intra.cfl_alpha_sign = 4;
  intra.cfl_alpha_idx = 0x21;
  intra.tx_size = 3;  // TX_32X32
  intra.current_qindex = 100;
  intra.cdef_level = 3;
  intra.cdef_strength = 1;
  block(0, 0, kB32x32, intra);
  MiInfo compound;  // NEW_NEWMV LAST + ALTREF, libaom COMPOUND_WEDGE (2)
  compound.mode = 24;
  compound.ref_frame = {1, 7};
  compound.mv_row = {-3, 5};
  compound.mv_col = {7, -9};
  compound.compound_type = 2;
  compound.tx_size = 2;  // TX_16X16: four TXBs
  compound.current_qindex = 104;
  block(0, 8, kB32x32, compound);
  MiInfo ibc;  // IntraBC, block vector (-16, -64) in 1/8 units
  ibc.intrabc = 1;
  ibc.ref_frame = {0, -1};
  ibc.mv_row = {-64, 0};
  ibc.mv_col = {-16, 0};
  ibc.skip = 1;
  ibc.tx_size = 3;
  block(8, 0, kB32x32, ibc);
  MiInfo interintra;  // NEARESTMV LAST2 + INTRA (interintra), OBMC
  interintra.mode = 13;
  interintra.ref_frame = {2, 0};
  interintra.mv_row = {4, 0};
  interintra.mv_col = {-4, 0};
  interintra.motion_mode = 1;
  interintra.skip = 1;
  interintra.tx_size = 3;
  block(8, 8, kB32x32, interintra);
  MiInfo global;  // GLOBALMV GOLDEN, ROTZOOM global motion
  global.mode = 15;
  global.ref_frame = {4, -1};
  global.mv_row = {1, 0};
  global.mv_col = {2, 0};
  global.skip = 1;
  global.tx_size = 4;
  block(0, 16, kB64x64, global);
  return d;
}

const fb::FrameAnalysis* analysis_of(const BlockData& d, int32_t w, int32_t h,
                                     std::unique_ptr<flatbuffers::FlatBufferBuilder>* keep) {
  ovt::aom::Frame f;
  f.decode_index = 5;
  f.frame_type = 4;  // Inter
  f.shown = true;
  f.coded_w = w;
  f.coded_h = h;
  f.render_w = w;
  f.render_h = h;
  f.blocks = d;
  std::map<std::string, ovt::aom::PlaneView> blobs;
  *keep = ovt::aom::build_frame_analysis(f, 0, "aom", "test", &blobs);
  flatbuffers::Verifier v((*keep)->GetBufferPointer(), (*keep)->GetSize());
  REQUIRE(fb::VerifyFrameAnalysisBuffer(v));
  return fb::GetFrameAnalysis((*keep)->GetBufferPointer());
}

// Validator rules a plugin buffer breaks until the host's post-processing (§7.5.3 A6) fills FrameInfo timing and
// the tiles' unit indices.
// Findings the plugin's output is expected to have, each waived only at its exact path:
// - V-002 frame.timebase_den and V-017 partitions[n].unit_index are filled by the host (§7.5.3 A6);
// - V-006 on the root rect when the coded size is not a multiple of 4: V-004 wants the root rect to equal
//   coded_size, V-006 wants every rect 4-aligned (spec question #360).
// Every other finding of those rules (coded_size, tile rects, partition_id range, ...) still fails the test.
bool expected_finding(const ovt::model::Violation& v, const fb::FrameAnalysis& fa) {
  static const std::regex kUnitIndex(R"(partitions\[[0-9]+\]\.unit_index)");
  if (v.rule == "V-002") {
    return v.path == "frame.timebase_den";
  }
  if (v.rule == "V-017") {
    return std::regex_match(v.path, kUnitIndex);
  }
  if (v.rule == "V-006" && v.path == "blocks[0].rect") {
    const fb::Rect* cs = fa.frame() != nullptr ? fa.frame()->coded_size() : nullptr;
    return cs != nullptr && (cs->w() % 4 != 0 || cs->h() % 4 != 0);
  }
  return false;
}

void check_valid(std::span<const uint8_t> buf) {
  const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(buf.data());
  for (const auto& v : ovt::model::validate_frame_analysis(buf, fb::Codec::AV1)) {
    INFO(v.rule << " " << v.path << ": " << v.message);
    CHECK(expected_finding(v, fa));
  }
}

const fb::Av1Block* av1(const fb::Block* b) {
  REQUIRE(b->ext() != nullptr);
  return flatbuffers::GetRoot<fb::Av1Block>(b->ext()->data());
}

}  // namespace

TEST_CASE("aom map_blocks: grid, blocks, MVs and refs of a hand-made frame", "[fast]") {
  std::unique_ptr<flatbuffers::FlatBufferBuilder> keep;
  const BlockData d = synthetic_frame();
  const fb::FrameAnalysis* fa = analysis_of(d, 66, 34, &keep);
  check_valid(std::span<const uint8_t>(keep->GetBufferPointer(), keep->GetSize()));
  const fb::CellGrid& g = *fa->grid();
  REQUIRE(g.cells_w() == 17);  // the MI padding column and row are dropped
  REQUIRE(g.cells_h() == 9);
  REQUIRE(g.block_id()->size() == 17 * 9);
  auto cell = [&](uint32_t x, uint32_t y) { return y * 17 + x; };

  // Intra V_PRED + CfL.
  CHECK(g.pred()->Get(cell(0, 0)) == 1);
  CHECK(g.intra_mode_y()->Get(cell(0, 0)) == 1);
  CHECK(g.intra_mode_c()->Get(cell(0, 0)) == 13);
  CHECK(g.ref0()->Get(cell(0, 0)) == -1);
  CHECK(g.tx_log2_w()->Get(cell(0, 0)) == 5);
  CHECK(g.cbf()->Get(cell(0, 0)) == 7);
  CHECK(g.filter_idx()->Get(cell(0, 0)) == 1);  // (level 3, strength 1) is strengths[1]
  const fb::Block* intra = fa->blocks()->Get(g.block_id()->Get(cell(0, 0)));
  CHECK(intra->kind() == fb::BlockKind::Block);
  CHECK(intra->rect()->w() == 32);
  CHECK(av1(intra)->CflAlphaU() == -3);
  CHECK(av1(intra)->CflAlphaV() == 2);
  CHECK(av1(intra)->UVMode() == 13);
  CHECK(av1(intra)->cdef_idx() == 1);

  // Compound NEW_NEWMV: MVs x2, both references, the libaom compound type remapped.
  const uint32_t cc = cell(8, 0);
  CHECK(g.pred()->Get(cc) == 2);
  CHECK(g.intra_mode_y()->Get(cc) == -1);
  CHECK(g.mv0_x()->Get(cc) == 14);
  CHECK(g.mv0_y()->Get(cc) == -6);
  CHECK(g.mv1_x()->Get(cc) == -18);
  CHECK(g.mv1_y()->Get(cc) == 10);
  CHECK(g.ref0()->Get(cc) == 1);
  CHECK(g.ref1()->Get(cc) == 7);
  CHECK(g.qp()->Get(cc) == 104);
  const fb::Block* comp = fa->blocks()->Get(g.block_id()->Get(cc));
  CHECK(av1(comp)->compound_type() == fb::Av1CompoundType::COMPOUND_WEDGE);
  CHECK(av1(comp)->comp_mode() == 1);
  CHECK_FALSE(comp->merge_or_direct());
  size_t txbs = 0;
  for (const uint32_t ch : *comp->children()) {
    CHECK(fa->blocks()->Get(ch)->kind() == fb::BlockKind::TXB);
    ++txbs;
  }
  CHECK(txbs == 4);

  // IntraBC: block vector in the grid, no reference, no MotionVector entry.
  const uint32_t ic = cell(0, 8);
  CHECK(g.pred()->Get(ic) == 4);
  CHECK(g.intra_mode_y()->Get(ic) == 32);
  CHECK(g.ref0()->Get(ic) == -1);
  CHECK(g.mv0_x()->Get(ic) == -32);
  CHECK(g.mv0_y()->Get(ic) == -128);
  CHECK(g.cbf()->Get(ic) == 0);
  CHECK(av1(fa->blocks()->Get(g.block_id()->Get(ic)))->use_intrabc_mv_y() == -64);

  // Interintra: ref_frame[1] == INTRA_FRAME is not a second reference.
  const uint32_t ii = cell(8, 8);
  CHECK(g.ref0()->Get(ii) == 2);
  CHECK(g.ref1()->Get(ii) == -1);
  CHECK(g.mv1_x()->Get(ii) == 0);
  const fb::Block* iib = fa->blocks()->Get(g.block_id()->Get(ii));
  CHECK(av1(iib)->interintra() == 1);
  CHECK(av1(iib)->compound_type() == fb::Av1CompoundType::COMPOUND_INTRA);
  CHECK(iib->merge_or_direct());
  CHECK(g.filter_idx()->Get(ii) == 1);  // same 64x64 CDEF unit as the intra block

  // The two-MI-wide superblock: GLOBALMV with ROTZOOM, all blocks skip -> cdef_idx not signalled.
  const uint32_t gc = cell(16, 0);
  CHECK(g.mv0_x()->Get(gc) == 4);
  CHECK(g.filter_idx()->Get(gc) == 0xFE);
  const fb::Block* sb = fa->blocks()->Get(g.block_id()->Get(gc));
  CHECK(sb->kind() == fb::BlockKind::SB);  // a 64x64 leaf superblock
  CHECK(sb->rect()->w() == 64);
  CHECK(av1(sb)->is_superblock() == 1);
  CHECK(av1(sb)->is_global() == 1);

  // MotionVectors: compound (2), interintra (1), global (1); none for IntraBC.
  REQUIRE(fa->mvs()->size() == 4);
  std::map<std::string, uint16_t> flags;
  for (const fb::MotionVector* m : *fa->mvs()) {
    flags[m->predictor()->str() + "/" + std::to_string(m->list())] = m->flags();
    CHECK(m->mvd_x() == -32768);
  }
  CHECK(flags.at("NEW_NEWMV/0") == 1);
  CHECK(flags.at("NEW_NEWMV/1") == 1);
  CHECK(flags.at("NEARESTMV+obmc/0") == 8);
  CHECK(flags.at("GLOBALMV+global/0") == 4);

  // refs[]: INTRA placeholder plus LAST..ALTREF with their DPB slots; usage counts both lists.
  REQUIRE(fa->refs()->size() == 8);
  CHECK(fa->refs()->Get(0)->name()->str() == "INTRA");
  CHECK(fa->refs()->Get(0)->slot() == -1);
  CHECK(fa->refs()->Get(7)->name()->str() == "ALTREF");
  CHECK(fa->refs()->Get(1)->slot() == 3);
  CHECK(fa->refs()->Get(4)->slot() == 0);
  CHECK(fa->refs()->Get(1)->usage_cells() == 64);  // 8 x 8 cells of the compound block
  CHECK(fa->refs()->Get(7)->usage_cells() == 64);
  CHECK(fa->refs()->Get(2)->usage_cells() == 8);  // the interintra block's one cell row inside the frame
  CHECK(fa->refs()->Get(4)->usage_cells() == 9);  // one cell column of the global block

  REQUIRE(fa->partitions()->size() == 1);
  CHECK(fa->partitions()->Get(0)->rect()->w() == 66);
  CHECK(fa->filter_params_type()->str() == "Av1FilterParams");
  const auto* fp = flatbuffers::GetRoot<fb::Av1FilterParams>(fa->filter_params()->data());
  CHECK(fp->gm()->size() == 7);
  CHECK(fp->cdef()->cdef_y_pri_strength()->Get(2) == 5);
}

TEST_CASE("aom map_blocks: no block data gives the empty grid only", "[fast]") {
  std::unique_ptr<flatbuffers::FlatBufferBuilder> keep;
  const fb::FrameAnalysis* fa = analysis_of(BlockData{}, 66, 34, &keep);
  CHECK(fa->grid()->cells_w() == 17);
  CHECK(fa->blocks() == nullptr);
  BlockData bad = synthetic_frame();
  bad.mi_cols = 4;  // smaller than the frame: refused with a warning
  const fb::FrameAnalysis* fb2 = analysis_of(bad, 66, 34, &keep);
  CHECK(fb2->blocks() == nullptr);
  REQUIRE(fb2->diagnostics()->size() == 1);
  CHECK(fb2->diagnostics()->Get(0)->severity() == fb::Severity::Warning);
}

// ---- the plugin against libaom inspect --------------------------------------------------------------------------

namespace {

const nlohmann::json& reference() {
  static const nlohmann::json j = [] {
    std::ifstream in(std::filesystem::path(OVT_AOM_TEST_DATA_DIR) / "inspect_reference.json");
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
  }();
  return j;
}

std::vector<int> unrle(const nlohmann::json& r) {
  std::vector<int> out;
  for (size_t i = 0; i + 1 < r.size(); i += 2) {
    out.insert(out.end(), r[i + 1].get<size_t>(), r[i].get<int>());
  }
  return out;
}

struct Analysed {
  std::vector<std::vector<uint8_t>> buffers;  // FrameAnalysis buffers in decode order
};

// Pushes the stream like the host (one OBU per call, hints from the frame OBUs) and keeps every FrameAnalysis.
Analysed analyse(const std::filesystem::path& path, size_t max_frames) {
  const auto tus = ovt::aom::test::read_ivf(path);
  ovt::aom::test::Instance inst;
  REQUIRE(inst.status == OVT_OK);
  Analysed out;
  int32_t next = 0;
  auto drain = [&] {
    for (;;) {
      ovt_Buffer buf{};
      REQUIRE(inst.api.pull(inst.dec, 0, &buf) == OVT_OK);
      if (buf.data == nullptr) {
        return;
      }
      const std::span<const uint8_t> s(buf.data, buf.size);
      out.buffers.emplace_back(s.begin(), s.end());
      inst.api.release(inst.dec, buf);
    }
  };
  for (const auto& tu : tus) {
    int32_t cur = -1;
    for (const auto& o : tu) {
      if (o.type == 3 || o.type == 6) {
        cur = next++;
      }
      const int32_t hint = cur >= 0 ? cur : next;
      REQUIRE(inst.api.push(inst.dec, o.bytes.data(), o.bytes.size(), hint, hint) == OVT_OK);
      drain();
    }
    if (out.buffers.size() >= max_frames) {
      break;
    }
  }
  REQUIRE(inst.api.flush(inst.dec) == OVT_OK);
  drain();
  return out;
}

// Compares one FrameAnalysis with inspect's layers of the same frame; returns the number of inter cells checked.
size_t compare_with_inspect(const fb::FrameAnalysis& fa, const nlohmann::json& ref) {
  const fb::CellGrid& g = *fa.grid();
  const auto mi_cols = ref["mi_cols"].get<uint32_t>();
  const auto& L = ref["layers"];
  const auto bsize = unrle(L["blockSize"]);
  const auto tx = unrle(L["transformSize"]);
  const auto ref0 = unrle(L["referenceFrame[0]"]);
  const auto ref1 = unrle(L["referenceFrame[1]"]);
  std::array<std::vector<int>, 4> mv;
  for (size_t k = 0; k < 4; ++k) {
    mv.at(k) = unrle(L["motionVectors[" + std::to_string(k) + "]"]);
  }
  const auto qindex = unrle(L["delta_q"]);
  const auto& slots = ref["ref_frame_idx"];
  size_t inter = 0;
  for (uint32_t y = 0; y < g.cells_h(); ++y) {
    for (uint32_t x = 0; x < g.cells_w(); ++x) {
      const uint32_t i = y * g.cells_w() + x;
      const size_t m = size_t{y} * mi_cols + x;
      // The leaf owning the cell has inspect's block size (leaves cover each cell exactly once: V-012, V-007).
      const fb::Block* leaf = fa.blocks()->Get(g.block_id()->Get(i));
      CHECK(static_cast<int>(av1(leaf)->bsize()) == bsize.at(m));
      CHECK(g.tx_log2_w()->Get(i) == ovt::aom::kTxLog2.at(static_cast<size_t>(tx.at(m))).w);
      CHECK(g.qp()->Get(i) == qindex.at(m));
      if (ref0.at(m) <= 0) {
        continue;
      }
      ++inter;
      CHECK(g.mv0_x()->Get(i) == 2 * mv[0].at(m));
      CHECK(g.mv0_y()->Get(i) == 2 * mv[1].at(m));
      CHECK(g.ref0()->Get(i) == ref0.at(m));
      const fb::RefSlot* rs = fa.refs()->Get(static_cast<uint32_t>(ref0.at(m)));
      CHECK(rs->name()->str() == ovt::aom::kRefNames.at(static_cast<size_t>(ref0.at(m))));
      CHECK(rs->slot() == slots.at(static_cast<size_t>(ref0.at(m) - 1)).get<int>());
      if (ref1.at(m) > 0) {
        CHECK(g.ref1()->Get(i) == ref1.at(m));
        CHECK(g.mv1_x()->Get(i) == 2 * mv[2].at(m));
        CHECK(g.mv1_y()->Get(i) == 2 * mv[3].at(m));
      } else {
        CHECK(g.ref1()->Get(i) == -1);
      }
    }
  }
  return inter;
}

}  // namespace

TEST_CASE("aom map_mi AT1: intra_mode_y of av1-1-b8-02-allintra frame 0 equals inspect's mode",
          "[fast][third_party][needs-corpus]") {
  const auto run = analyse(ovt::test::corpus_or_skip("aom_av1_1_b8_02_allintra"), 1);
  REQUIRE(!run.buffers.empty());
  const fb::FrameAnalysis* fa = fb::GetFrameAnalysis(run.buffers[0].data());
  const auto& ref = reference()["streams"]["aom_av1_1_b8_02_allintra"][0];
  const auto mode = unrle(ref["layers"]["mode"]);
  const auto mi_cols = ref["mi_cols"].get<size_t>();
  const fb::CellGrid& g = *fa->grid();
  REQUIRE(g.cells_w() == 88);
  REQUIRE(g.cells_h() == 72);
  size_t differ = 0;
  for (uint32_t y = 0; y < g.cells_h(); ++y) {
    for (uint32_t x = 0; x < g.cells_w(); ++x) {
      differ += g.intra_mode_y()->Get(y * g.cells_w() + x) == mode.at(size_t{y} * mi_cols + x) ? 0U : 1U;
    }
  }
  CHECK(differ == 0);
  compare_with_inspect(*fa, ref);
  check_valid(run.buffers[0]);
}

TEST_CASE("aom map_mi AT2: MVs and references of av1-1-b8-05-mv equal inspect's", "[fast][third_party][needs-corpus]") {
  const auto run = analyse(ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv"), 100);
  const auto& frames = reference()["streams"]["aom_av1_1_b8_05_mv"];
  REQUIRE(run.buffers.size() == frames.size());  // one FrameAnalysis per frame with tile data
  size_t inter = 0;
  for (size_t f = 0; f < frames.size(); ++f) {
    INFO("frame " << f);
    const fb::FrameAnalysis* fa = fb::GetFrameAnalysis(run.buffers[f].data());
    inter += compare_with_inspect(*fa, frames[f]);
    check_valid(run.buffers[f]);
  }
  CHECK(inter > 10000);  // three inter frames of 88 x 72 cells
}

TEST_CASE("aom map_mi AT3 (plugin): 128x128 superblocks and tiles of syn_av1_tiles_cdef_lr",
          "[fast][third_party][needs-corpus]") {
  const auto& frames = reference()["streams"]["syn_av1_tiles_cdef_lr"];
  const auto run = analyse(ovt::test::corpus_or_skip("syn_av1_tiles_cdef_lr"), frames.size());
  REQUIRE(run.buffers.size() >= frames.size());
  std::set<uint8_t> partitions;
  for (size_t f = 0; f < frames.size(); ++f) {
    INFO("frame " << f);
    const fb::FrameAnalysis* fa = fb::GetFrameAnalysis(run.buffers[f].data());
    compare_with_inspect(*fa, frames[f]);
    check_valid(run.buffers[f]);
    const fb::Block* sb = fa->blocks()->Get(fa->blocks()->Get(0)->children()->Get(0));
    CHECK(sb->kind() == fb::BlockKind::SB);
    CHECK(sb->rect()->w() == 128);
    CHECK(fa->partitions()->size() > 1);  // --tile-columns=2 --tile-rows=1
    for (const fb::Block* b : *fa->blocks()) {
      if (b->kind() == fb::BlockKind::Block || b->kind() == fb::BlockKind::SB) {
        partitions.insert(b->partition_type());
      }
    }
  }
  CHECK(partitions.size() >= 4);  // NONE, HORZ, VERT, SPLIT at least
}

TEST_CASE("aom map_mi: every frame of the fast libaom vectors passes the validator",
          "[fast][third_party][needs-corpus]") {
  // Odd sizes (MI padding), IntraBC and palette, film grain, 10-bit, CDF update, hidden frames, SVC.
  const auto id = GENERATE(as<std::string>{}, "aom_av1_1_b8_01_size_16x16", "aom_av1_1_b8_01_size_196x196",
                           "aom_av1_1_b8_01_size_226x226", "aom_av1_1_b8_04_cdfupdate", "aom_av1_1_b8_06_mfmv",
                           "aom_av1_1_b8_16_intra_only_intrabc_extreme_dv", "aom_av1_1_b8_22_svc_L1T2",
                           "aom_av1_1_b8_23_film_grain_50", "aom_av1_1_b10_00_quantizer_10");
  INFO(id);
  const auto run = analyse(ovt::test::corpus_or_skip(id), 12);
  REQUIRE(!run.buffers.empty());
  for (size_t f = 0; f < run.buffers.size(); ++f) {
    INFO("frame " << f);
    check_valid(run.buffers[f]);
    const fb::FrameAnalysis* fa = fb::GetFrameAnalysis(run.buffers[f].data());
    REQUIRE(fa->blocks() != nullptr);
    CHECK(fa->blocks()->size() > 1);
    // No partition ambiguity, no uncovered cell. The only diagnostics allowed are the film grain vector's Info note
    // (WI-2.158) and the SVC vector's (three tile groups per frame) accounting note (WI-2.157), on those vectors.
    for (const fb::Diagnostic* d : *fa->diagnostics()) {
      CHECK(d->severity() == fb::Severity::Info);
      if (id == "aom_av1_1_b8_22_svc_L1T2") {
        CHECK(d->message()->str() == "accounting covers last tile group only");
      } else {
        CHECK(id == "aom_av1_1_b8_23_film_grain_50");
        CHECK(d->code()->str() == "DEC-AV1-0001");
        CHECK(d->message()->str() == "film grain present (not applied)");
      }
    }
  }
}

TEST_CASE("aom map_mi: the same stream gives byte-identical FrameAnalysis buffers",
          "[fast][third_party][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv");
  CHECK(analyse(path, 100).buffers == analyse(path, 100).buffers);  // determinism, §0.7.5
}

TEST_CASE("aom partition AT3: inspect's blockSize layouts are rebuilt exactly", "[fast]") {
  // Superblock size per stream (sb_size of the sequence header): syn_av1_tiles_cdef_lr is encoded with
  // --sb-size=128 (chapter 14 recipe 13); the libaom test vectors use 64x64.
  const std::map<std::string, int32_t> sb_mi = {
      {"aom_av1_1_b8_02_allintra", 16}, {"aom_av1_1_b8_05_mv", 16}, {"syn_av1_tiles_cdef_lr", 32}};
  for (const auto& [stream, sb] : sb_mi) {
    std::set<uint8_t> partitions;
    const auto& frames = reference()["streams"][stream];
    REQUIRE(!frames.empty());
    for (size_t f = 0; f < frames.size(); ++f) {
      INFO(stream << " frame " << f);
      const auto layer = unrle(frames[f]["layers"]["blockSize"]);
      Grid g(frames[f]["mi_rows"].get<int32_t>(), frames[f]["mi_cols"].get<int32_t>());
      REQUIRE(layer.size() == g.bsize.size());
      std::transform(layer.begin(), layer.end(), g.bsize.begin(), [](int b) { return static_cast<uint8_t>(b); });
      for (int32_t r = 0; r < g.rows; r += sb) {
        for (int32_t c = 0; c < g.cols; c += sb) {
          const auto nodes = rebuild(g, r, c, sb);
          check_cover(nodes, g, r, c, sb);
          for (const PartNode& n : nodes) {
            CHECK_FALSE(n.ambiguous);
            partitions.insert(n.partition);
          }
        }
      }
    }
    if (stream == "syn_av1_tiles_cdef_lr") {
      CHECK(partitions.contains(part(ovt::aom::Partition::HorzA)));
      CHECK(partitions.contains(part(ovt::aom::Partition::Vert4)));
    }
  }
}
