// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Tests of the libaom extension patches of ovt-plugin-aom (WI-2.159, chapter 07 §7.6.1 extension table): the mapping
// of the extension fields on a hand-made frame, and the plugin's output for libaom test vectors against `inspect`
// built with patch 0001 (data/inspect_ext_reference.json, written by data/make_inspect_ext_reference.py).
// AT1: av1-1-b8-23-film_grain-50, Av1Block.AngleDeltaY == inspect -ad for every intra directional block of every
//      frame (frame 5 of the WI has none; see the test).
// AT2: warped-motion blocks, Av1Block.LocalWarpParams == inspect -wm. SPEC-ASSUMPTION(#480): the WI names Argon
//      streams, which are not in the corpus (nightly-only, not fetched); av1-1-b8-05-mv and film_grain-50 have
//      thousands of WARPED_CAUSAL blocks.
// AT3: frames whose headers disable CDEF / loop restoration / deblocking: PreSaoOrCdef == PreLoopRestoration /
//      PreLoopRestoration == Reconstruction / PreDeblock == PreSaoOrCdef, and the stages differ where the filter
//      ran. Same SPEC-ASSUMPTION: the libaom fast vectors stand in for Argon.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "aom_test_util.inc"
#include "block_data.hpp"
#include "map_mi.hpp"
#include "map_mi_ext.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace fb = ovt::fb;
using ovt::aom::BlockData;
using ovt::aom::MiInfo;

namespace {

const fb::Av1Block* av1(const fb::Block* b) {
  REQUIRE(b->ext() != nullptr);
  return flatbuffers::GetRoot<fb::Av1Block>(b->ext()->data());
}

// ---- hand-made frame -------------------------------------------------------------------------------------------

// One 64x64 superblock (16x16 MIs, a 64x64 frame) split PARTITION_VERT: the left 32x64 block is a compound wedge
// block (NEW_NEARESTMV: list 0 codes a difference), the right one a single-reference warped NEWMV block. Each block
// has two 32x32 transform units with different types.
BlockData ext_frame() {
  BlockData d;
  d.valid = true;
  d.ext = true;
  d.mi_rows = 16;
  d.mi_cols = 16;
  d.mi.resize(256);
  d.intra_frame = false;
  d.ref_frame_idx = {0, 1, 2, 3, 4, 5, 6};
  d.cdef_strengths = {0, 5, 9, 13};
  d.cdef_uv_strengths = {0, 1, 2, 3};
  d.frame_width = 64;
  d.upscaled_width = 64;
  d.tile_col_starts = {0, 16};
  d.tile_row_starts = {0, 16};
  for (int32_t r = 0; r < 16; ++r) {
    for (int32_t c = 0; c < 16; ++c) {
      MiInfo& m = d.mi[(static_cast<size_t>(r) * 16) + static_cast<size_t>(c)];
      m.bsize = 10;               // BLOCK_32X64
      m.partition = 2;            // PARTITION_VERT
      m.tx_size = 3;              // TX_32X32
      m.tx_type = r < 8 ? 1 : 9;  // two transform units per block: ADST_DCT, IDTX
      m.cdef_idx = 2;
      m.filter = {0, 0};
      m.mv_row = {12, -6};
      m.mv_col = {-8, 4};
      m.ref_mv_idx = 1;
      if (c < 8) {    // compound wedge, NEW_NEARESTMV: list 0 coded a difference
        m.mode = 20;  // NEW_NEARESTMV
        m.ref_frame = {1, 7};
        m.compound_type = 2;  // COMPOUND_WEDGE
        m.wedge_index = 5;
        m.wedge_sign = 1;
        m.comp_group_idx = 1;
        m.compound_idx = 1;
        m.mvd = {{{4, -2}, {-32768, -32768}}};
        m.mv_joint = {3, -1};
      } else {        // warped NEWMV
        m.mode = 16;  // NEWMV
        m.ref_frame = {2, -1};
        m.motion_mode = 2;  // WARPED_CAUSAL
        m.wm_type = 3;
        m.wm_invalid = 0;
        m.wm_params = {100, -200, 65536, 12, -34, 65530};
        m.mvd = {{{0, 8}, {-32768, -32768}}};
        m.mv_joint = {1, -1};
      }
    }
  }
  d.filters.lf_level = {10, 12, 3, 4};
  d.filters.lf_sharpness = 2;
  d.filters.lr_type = {1, 2, 0};
  d.filters.lr_unit_size = {64, 32, 0};
  d.filters.lr_horz = {1, 1, 0};
  d.filters.lr_vert = {1, 1, 0};
  d.filters.lr_units.at(0) = {ovt::aom::LrUnit{1, {3, -7, 15}, {2, -5, 11}, 0, {}}};
  d.filters.lr_units.at(1) = {ovt::aom::LrUnit{2, {}, {}, 13, {-32, 96}}};
  return d;
}

}  // namespace

TEST_CASE("aom map_mi_ext: extension fields of a hand-made frame", "[fast]") {
  const BlockData d = ext_frame();
  flatbuffers::FlatBufferBuilder fbb(4096);
  const auto mapped = ovt::aom::map_blocks(fbb, d, 64, 64);
  CHECK(mapped.warnings.empty());  // the partition check agrees with the reconstructed tree
  fb::FrameAnalysisBuilder fa(fbb);
  fa.add_grid(mapped.grid);
  fa.add_blocks(mapped.blocks);
  fa.add_mvs(mapped.mvs);
  fa.add_filter_params(mapped.filter_params);
  fbb.Finish(fa.Finish());
  const fb::FrameAnalysis* out = fb::GetFrameAnalysis(fbb.GetBufferPointer());

  const fb::CellGrid& g = *out->grid();
  REQUIRE(g.tx_type() != nullptr);
  CHECK(g.tx_type()->Get(0) == 1);       // cell (0, 0)
  CHECK(g.tx_type()->Get(8 * 16) == 9);  // cell (0, 8): second transform unit
  CHECK(g.filter_idx()->Get(0) == 2);    // cdef_idx read directly

  // Leaves: blocks[0] frame, [1] superblock, [2] left (+ 2 TXBs), [5] right.
  const fb::Block* left = out->blocks()->Get(g.block_id()->Get(0));
  const fb::Block* right = out->blocks()->Get(g.block_id()->Get(8));
  const fb::Av1Block* l = av1(left);
  const fb::Av1Block* r = av1(right);
  CHECK(l->TxType() == fb::Av1TxType::ADST_DCT);
  CHECK(l->cdef_idx() == 2);
  CHECK(l->drl_mode_idx() == 1);
  CHECK(l->comp_group_idx() == 1);
  CHECK(l->compound_idx() == 1);
  CHECK(l->wedge_index() == 5);
  CHECK(l->wedge_sign() == 1);
  REQUIRE(l->Mvd() != nullptr);
  CHECK(std::vector<int16_t>(l->Mvd()->begin(), l->Mvd()->end()) == std::vector<int16_t>{4, -2, -32768, -32768});
  REQUIRE(l->mv_joint() != nullptr);
  CHECK(std::vector<uint8_t>(l->mv_joint()->begin(), l->mv_joint()->end()) == std::vector<uint8_t>{3});
  CHECK(l->LocalWarpParams() == nullptr);
  CHECK(r->LocalValid() == 1);
  REQUIRE(r->LocalWarpParams() != nullptr);
  CHECK(std::vector<int32_t>(r->LocalWarpParams()->begin(), r->LocalWarpParams()->end()) ==
        std::vector<int32_t>{100, -200, 65536, 12, -34, 65530});
  CHECK(r->skip_mode() == 0);

  // Av1Txb.TxType per transform unit.
  REQUIRE(left->children() != nullptr);
  REQUIRE(left->children()->size() == 2);
  const auto* txb = flatbuffers::GetRoot<fb::Av1Txb>(out->blocks()->Get(left->children()->Get(1))->ext()->data());
  CHECK(txb->TxType() == fb::Av1TxType::IDTX);

  // MotionVector.mvd in 1/16 (x2), -32768 for lists without NEWMV.
  REQUIRE(out->mvs()->size() == 3);  // left: two lists, right: one
  CHECK(out->mvs()->Get(0)->mvd_x() == -4);
  CHECK(out->mvs()->Get(0)->mvd_y() == 8);
  CHECK(out->mvs()->Get(1)->mvd_x() == -32768);
  CHECK(out->mvs()->Get(2)->mvd_x() == 16);
  CHECK(out->mvs()->Get(2)->mvd_y() == 0);

  // Frame-level loop filter and loop restoration.
  const auto* fp = flatbuffers::GetRoot<fb::Av1FilterParams>(out->filter_params()->data());
  REQUIRE(fp->loop_filter_level() != nullptr);
  CHECK(std::vector<uint8_t>(fp->loop_filter_level()->begin(), fp->loop_filter_level()->end()) ==
        std::vector<uint8_t>{10, 12, 3, 4});
  CHECK(fp->loop_filter_sharpness() == 2);
  REQUIRE(fp->FrameRestorationType()->size() == 3);
  CHECK(fp->FrameRestorationType()->Get(1) == fb::Av1RestorationType::RESTORE_SGRPROJ);
  REQUIRE(fp->lr_units()->size() == 2);
  const fb::Av1LrUnit* y = fp->lr_units()->Get(0);
  CHECK(y->restoration_type() == fb::Av1RestorationType::RESTORE_WIENER);
  CHECK(std::vector<int16_t>(y->LrWiener()->begin(), y->LrWiener()->end()) ==
        std::vector<int16_t>{3, -7, 15, 2, -5, 11});
  CHECK(y->rect()->w() == 64);
  const fb::Av1LrUnit* u = fp->lr_units()->Get(1);
  CHECK(u->plane() == 1);
  CHECK(u->LrSgrSet() == 13);
  CHECK(u->rect()->w() == 64);  // the 32x32 chroma unit, in luma samples
  CHECK(std::vector<int16_t>(u->LrSgrXqd()->begin(), u->LrSgrXqd()->end()) == std::vector<int16_t>{-32, 96});
}

TEST_CASE("aom map_mi_ext: grid tx_type and mvd sentinels", "[fast]") {
  MiInfo m;
  m.tx_type = 4;
  CHECK(ovt::aom::grid_tx_type(m) == 4);
  m.skip = 1;
  CHECK(ovt::aom::grid_tx_type(m) == 0xFF);
  m.skip = 0;
  m.tx_type = -1;
  CHECK(ovt::aom::grid_tx_type(m) == 0xFF);
  CHECK(ovt::aom::mvd_of(m, 0).x == -32768);
  m.mvd.at(1) = {-3, 5};
  CHECK(ovt::aom::mvd_of(m, 1).x == 10);
  CHECK(ovt::aom::mvd_of(m, 1).y == -6);
  m.cdef_idx = 7;
  CHECK(ovt::aom::ext_cdef_idx(m, 8) == 7);
  CHECK(ovt::aom::ext_cdef_idx(m, 4) == 0xFF);
}

// ---- the plugin against patched inspect ------------------------------------------------------------------------

namespace {

constexpr uint32_t kAllStages = (1U << 0) | (1U << 3) | (1U << 4) | (1U << 5);

const nlohmann::json& reference() {
  static const nlohmann::json j = [] {
    std::ifstream in(std::filesystem::path(OVT_AOM_TEST_DATA_DIR) / "inspect_ext_reference.json");
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

// One pulled frame: the FrameAnalysis bytes and each plane's visible samples by "<stage>/<plane>".
struct Pulled {
  std::vector<uint8_t> fa;
  std::map<std::string, std::vector<uint8_t>> planes;
  std::map<std::string, std::array<uint32_t, 2>> sizes;  // width, height
};

Pulled read_frame(const ovt_AnalysisDecoderApi& api, ovt_AnalysisDecoder* dec, ovt_Buffer buf) {
  flatbuffers::Verifier verifier(buf.data, buf.size);
  REQUIRE(fb::VerifyFrameAnalysisBuffer(verifier));
  Pulled out;
  const std::span<const uint8_t> s(buf.data, buf.size);
  out.fa.assign(s.begin(), s.end());
  const fb::FrameAnalysis* fa = fb::GetFrameAnalysis(buf.data);
  for (const fb::PlaneRef* p : *fa->planes()) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    REQUIRE(api.plane(dec, p->blob_id()->c_str(), &data, &size) == OVT_OK);
    const size_t row = size_t{p->width()} * p->sample_bytes();
    const std::span<const uint8_t> bytes(data, size);
    std::vector<uint8_t> rows;
    for (uint32_t y = 0; y < p->height(); ++y) {
      const auto r = bytes.subspan(size_t{p->stride_bytes()} * y, row);
      rows.insert(rows.end(), r.begin(), r.end());
    }
    const std::string key = std::to_string(static_cast<int>(p->stage())) + "/" + std::to_string(p->plane());
    out.planes[key] = std::move(rows);
    out.sizes[key] = {p->width(), p->height()};
  }
  return out;
}

std::vector<Pulled> analyse(const std::filesystem::path& path, uint32_t mask) {
  const auto tus = ovt::aom::test::read_ivf(path);
  ovt::aom::test::Instance inst;
  REQUIRE(inst.status == OVT_OK);
  std::vector<Pulled> out;
  int32_t next = 0;
  auto drain = [&] {
    for (;;) {
      ovt_Buffer buf{};
      REQUIRE(inst.api.pull(inst.dec, mask, &buf) == OVT_OK);
      if (buf.data == nullptr) {
        return;
      }
      out.push_back(read_frame(inst.api, inst.dec, buf));
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
  }
  REQUIRE(inst.api.flush(inst.dec) == OVT_OK);
  drain();
  return out;
}

// The Av1Block of the leaf that covers MI (row, col).
const fb::Av1Block* leaf_at(const fb::FrameAnalysis& fa, uint32_t row, uint32_t col) {
  const fb::CellGrid& g = *fa.grid();
  return av1(fa.blocks()->Get(g.block_id()->Get(row * g.cells_w() + col)));
}

// True for a leaf whose origin is MI (row, col).
bool is_origin(const fb::FrameAnalysis& fa, uint32_t row, uint32_t col) {
  const fb::Av1Block* b = leaf_at(fa, row, col);
  return b->MiRow() == row && b->MiCol() == col;
}

}  // namespace

TEST_CASE("aom map_mi_ext AT1: AngleDeltaY of film_grain-50 equals inspect -ad", "[fast][third_party][needs-corpus]") {
  const auto run = analyse(ovt::test::corpus_or_skip("aom_av1_1_b8_23_film_grain_50"), 0);
  const auto& frames = reference()["streams"]["aom_av1_1_b8_23_film_grain_50"];
  REQUIRE(run.size() == frames.size());
  // "Frame 5" of the WI is inspect's sixth frame record (decode index 6: decode index 5 is a show_existing_frame
  // header without blocks). That inter frame has no intra directional block, so every frame is compared; the
  // key frame has about 2 500 directional MIs.
  // SPEC-ASSUMPTION(#480): AT1 checks every frame of the stream, not frame 5 alone.
  REQUIRE(fb::GetFrameAnalysis(run.at(5).fa.data())->frame()->id()->decode_index() == 6);
  size_t directional = 0;
  size_t nonzero = 0;
  for (size_t f = 0; f < frames.size(); ++f) {
    const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(run[f].fa.data());
    const auto& ref = frames[f];
    const auto mode = unrle(ref["layers"]["mode"]);
    const auto ad_y = unrle(ref["layers"]["angle_delta[0]"]);
    const auto ad_uv = unrle(ref["layers"]["angle_delta[1]"]);
    const auto mi_cols = ref["mi_cols"].get<size_t>();
    for (uint32_t y = 0; y < fa.grid()->cells_h(); ++y) {
      for (uint32_t x = 0; x < fa.grid()->cells_w(); ++x) {
        const size_t m = size_t{y} * mi_cols + x;
        const fb::Av1Block* b = leaf_at(fa, y, x);
        if (!is_origin(fa, y, x) || b->is_inter() == 1 || mode.at(m) < 1 || mode.at(m) > 8) {  // V_PRED..D67_PRED
          continue;
        }
        ++directional;
        nonzero += ad_y.at(m) != 0 ? 1U : 0U;
        INFO("frame " << f << " MI (" << y << ", " << x << ")");
        CHECK(b->AngleDeltaY() == ad_y.at(m));
        CHECK(b->AngleDeltaUV() == ad_uv.at(m));
      }
    }
  }
  CHECK(directional > 100);
  CHECK(nonzero > 0);
}

TEST_CASE("aom map_mi_ext AT2: LocalWarpParams of warped blocks equal inspect -wm",
          "[fast][third_party][needs-corpus]") {
  for (const char* id : {"aom_av1_1_b8_05_mv", "aom_av1_1_b8_23_film_grain_50"}) {
    INFO(id);
    const auto run = analyse(ovt::test::corpus_or_skip(id), 0);
    const auto& frames = reference()["streams"][id];
    REQUIRE(run.size() == frames.size());
    size_t warped = 0;
    for (size_t f = 0; f < frames.size(); ++f) {
      const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(run[f].fa.data());
      const auto& L = frames[f]["layers"];
      const auto motion = unrle(L["motion_mode"]);
      std::array<std::vector<int>, 6> wm;
      for (size_t k = 0; k < 6; ++k) {
        wm.at(k) = unrle(L["wm_params[" + std::to_string(k) + "]"]);
      }
      const auto mi_cols = frames[f]["mi_cols"].get<size_t>();
      for (uint32_t y = 0; y < fa.grid()->cells_h(); ++y) {
        for (uint32_t x = 0; x < fa.grid()->cells_w(); ++x) {
          const size_t m = size_t{y} * mi_cols + x;
          if (motion.at(m) != 2 || !is_origin(fa, y, x)) {
            continue;
          }
          ++warped;
          const fb::Av1Block* b = leaf_at(fa, y, x);
          INFO("frame " << f << " MI (" << y << ", " << x << ")");
          REQUIRE(b->LocalWarpParams() != nullptr);
          REQUIRE(b->LocalWarpParams()->size() == 6);
          for (uint32_t k = 0; k < 6; ++k) {
            CHECK(b->LocalWarpParams()->Get(k) == wm.at(k).at(m));
          }
        }
      }
    }
    CHECK(warped > 10);
  }
}

TEST_CASE("aom map_mi_ext: mvd, tx_type and cdef_idx of av1-1-b8-05-mv equal patched inspect",
          "[fast][third_party][needs-corpus]") {
  const auto run = analyse(ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv"), 0);
  const auto& frames = reference()["streams"]["aom_av1_1_b8_05_mv"];
  REQUIRE(run.size() == frames.size());
  size_t coded_mvd = 0;
  for (size_t f = 0; f < frames.size(); ++f) {
    const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(run[f].fa.data());
    const fb::CellGrid& g = *fa.grid();
    const auto& L = frames[f]["layers"];
    const auto tx_type = unrle(L["transformType"]);
    std::array<std::vector<int>, 4> mvd;
    for (size_t k = 0; k < 4; ++k) {
      mvd.at(k) = unrle(L["mvd[" + std::to_string(k) + "]"]);
    }
    const auto mi_cols = frames[f]["mi_cols"].get<size_t>();
    for (uint32_t y = 0; y < g.cells_h(); ++y) {
      for (uint32_t x = 0; x < g.cells_w(); ++x) {
        const size_t m = size_t{y} * mi_cols + x;
        const uint32_t i = y * g.cells_w() + x;
        INFO("frame " << f << " MI (" << y << ", " << x << ")");
        CHECK(g.tx_type()->Get(i) == (tx_type.at(m) < 0 ? 0xFF : tx_type.at(m)));
        if (!is_origin(fa, y, x) || g.ref0()->Get(i) <= 0) {
          continue;
        }
        const fb::Av1Block* b = leaf_at(fa, y, x);
        REQUIRE(b->Mvd() != nullptr);
        for (uint32_t k = 0; k < 4; ++k) {
          CHECK(b->Mvd()->Get(k) == mvd.at(k).at(m));
        }
        coded_mvd += mvd[0].at(m) != -32768 ? 1U : 0U;
      }
    }
    const auto& vfa = run[f].fa;
    for (const auto& v : ovt::model::validate_frame_analysis(vfa, fb::Codec::AV1)) {
      INFO(v.rule << " " << v.path << ": " << v.message);
      CHECK((v.rule == "V-002" || v.rule == "V-017" || v.rule == "V-006"));  // host fills these (§7.5.3 A6)
    }
  }
  CHECK(coded_mvd > 0);
}

namespace {

// Checks that every cell of the luma TXB @p r (clipped to the grid) has grid.tx_type @p tx_type; returns the
// number of cells checked.
size_t check_txb_cells(const fb::FrameAnalysis& fa, const fb::CellGrid& g, const fb::Rect& r, uint8_t tx_type) {
  const auto y0 = static_cast<uint32_t>(r.y() / 4);
  const auto x0 = static_cast<uint32_t>(r.x() / 4);
  const uint32_t y1 = std::min(static_cast<uint32_t>((r.y() + r.h()) / 4), g.cells_h());
  const uint32_t x1 = std::min(static_cast<uint32_t>((r.x() + r.w()) / 4), g.cells_w());
  size_t n = 0;
  for (uint32_t y = y0; y < y1; ++y) {
    for (uint32_t x = x0; x < x1; ++x) {
      INFO("decode index " << fa.frame()->id()->decode_index() << " MI (" << y << ", " << x << ")");
      CHECK(g.tx_type()->Get((y * g.cells_w()) + x) == tx_type);
      ++n;
    }
  }
  return n;
}

}  // namespace

// Var-tx: an inter block whose transform units have different sizes reads each MI's tx_type at the top-left of
// its own unit (inter_tx_size[], not the block's last tx_size), so every MI inside one luma TXB carries that
// TXB's TxType, and the TxType is what tx_type_map holds at the TXB origin.
TEST_CASE("aom map_mi_ext: every MI of a var-tx transform unit carries the unit's tx_type",
          "[fast][third_party][needs-corpus]") {
  size_t mixed = 0;
  size_t checked = 0;
  // The libaom fast vectors code no inter block with mixed transform unit sizes; syn_av1_tiles_cdef_lr (aomenc
  // --cpu-used=4) has some, so it comes first and the test skips without it.
  for (const char* id : {"syn_av1_tiles_cdef_lr", "aom_av1_1_b8_05_mv"}) {
    INFO(id);
    for (const Pulled& p : analyse(ovt::test::corpus_or_skip(id), 0)) {
      const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(p.fa.data());
      const fb::CellGrid& g = *fa.grid();
      std::map<uint32_t, std::set<int>> sizes;  // leaf id -> TxSize of its TXBs
      for (const fb::Block* b : *fa.blocks()) {
        if (b->kind() != fb::BlockKind::TXB) {  // intra leaves have one luma TX size: "mixed" counts var-tx only
          continue;
        }
        const auto* t = flatbuffers::GetRoot<fb::Av1Txb>(b->ext()->data());
        sizes[static_cast<uint32_t>(b->parent())].insert(static_cast<int>(t->txSz()));
        if (t->TxType() == fb::Av1TxType::Av1TxType_Unknown) {
          continue;
        }
        checked += check_txb_cells(fa, g, *b->rect(), static_cast<uint8_t>(t->TxType()));
      }
      for (const auto& [leaf, s] : sizes) {
        mixed += s.size() > 1 ? 1U : 0U;
      }
    }
  }
  CHECK(mixed > 0);  // var-tx inter blocks with mixed unit sizes were seen
  CHECK(checked > 0);
}

namespace {

// Counts of the frames (luma plane) where AT3 saw a filter skipped or run.
struct StageTally {
  size_t no_cdef = 0;
  size_t no_lr = 0;
  size_t cdef_ran = 0;
  size_t lf_ran = 0;
};

bool lr_all_off(const fb::Av1FilterParams& fp) {
  bool off = true;
  for (const auto t : *fp.FrameRestorationType()) {
    off = off && t == fb::Av1RestorationType::RESTORE_NONE;
  }
  return off;
}

// AT3 on one frame: each pre-filter stage equals the next stage wherever the frame disables that filter.
void check_stages(const Pulled& p, const fb::FrameAnalysis& fa, const fb::Av1FilterParams& fp, StageTally& t) {
  const bool superres = fp.use_superres() != 0;
  const bool lr_off = lr_all_off(fp);
  const bool lf_off = fp.loop_filter_level()->Get(0) == 0 && fp.loop_filter_level()->Get(1) == 0;
  const bool shown = fa.frame()->shown();
  for (const std::string pl : {"0", "1", "2"}) {
    if (!p.planes.contains("3/" + pl)) {
      continue;
    }
    const bool luma = pl == "0";
    const auto& pre_lf = p.planes.at("3/" + pl);
    const auto& pre_cdef = p.planes.at("4/" + pl);
    const auto& pre_lr = p.planes.at("5/" + pl);
    if (luma) {  // deblocking runs iff a luma level is set
      CHECK((pre_lf == pre_cdef) == lf_off);
      t.lf_ran += lf_off ? 0U : 1U;
    }
    if (fp.enable_cdef() == 0 && !superres) {
      CHECK(pre_cdef == pre_lr);
      t.no_cdef += luma ? 1U : 0U;
    } else if (!superres && luma && pre_cdef != pre_lr) {
      ++t.cdef_ran;
    }
    if (lr_off && shown) {
      CHECK(pre_lr == p.planes.at("0/" + pl));
      t.no_lr += luma ? 1U : 0U;
    }
  }
}

}  // namespace

TEST_CASE("aom map_mi_ext AT3: pre-filter stages equal their successor where the frame disables the filter",
          "[fast][third_party][needs-corpus]") {
  StageTally t;
  for (const char* id :
       {"aom_av1_1_b8_02_allintra", "aom_av1_1_b8_05_mv", "aom_av1_1_b8_06_mfmv", "aom_av1_1_b8_23_film_grain_50",
        "aom_av1_1_b8_01_size_66x66", "aom_av1_1_b8_16_intra_only_intrabc_extreme_dv"}) {
    INFO(id);
    for (const Pulled& p : analyse(ovt::test::corpus_or_skip(id), kAllStages)) {
      const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(p.fa.data());
      const auto* fp = flatbuffers::GetRoot<fb::Av1FilterParams>(fa.filter_params()->data());
      INFO("decode index " << fa.frame()->id()->decode_index());
      // Snapshots exist for every frame, hidden ones included; Reconstruction only for shown frames.
      REQUIRE(p.planes.contains("3/0"));
      REQUIRE(p.planes.contains("4/0"));
      REQUIRE(p.planes.contains("5/0"));
      CHECK(p.planes.contains("0/0") == fa.frame()->shown());
      check_stages(p, fa, *fp, t);
    }
  }
  CHECK(t.no_cdef > 0);
  CHECK(t.no_lr > 0);
  CHECK(t.lf_ran > 0);
  CHECK(t.cdef_ran > 0);
}

TEST_CASE("aom map_mi_ext: snapshot stages come out only when asked for", "[fast][third_party][needs-corpus]") {
  const auto run = analyse(ovt::test::corpus_or_skip("aom_av1_1_b8_01_size_16x16"), 1U << 4);
  REQUIRE(!run.empty());
  for (const Pulled& p : run) {
    REQUIRE(p.planes.size() == 3);
    for (const auto& [key, rows] : p.planes) {
      CHECK(key.starts_with("4/"));
      const auto& wh = p.sizes.at(key);
      CHECK(rows.size() == size_t{wh[0]} * wh[1]);  // 8-bit stream: one byte per sample
    }
  }
}
