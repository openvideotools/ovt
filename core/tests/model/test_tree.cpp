// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.084 acceptance tests 1 (tree part) and 3: TreeWalker and ext_as on the §6.10 example (chapter 06).
//
// The golden fixture tiny16_hevc.fa arrives with WI-0.093. Until then this file builds the parts of the
// §6.10 example that the walker reads: the 4×4 cell grid, the 14 blocks with their rects, kinds, parents and
// children, and the HevcCu / HevcPu extension buffers.
// SPEC-ASSUMPTION(#98): acceptance test 3 runs on this in-test copy of §6.10 until WI-0.093 lands.

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include "ovt/cxx/model/tree.hpp"

namespace {

using ovt::fb::BlockKind;

struct BlockSpec {
  std::uint32_t id;
  std::int32_t parent;
  std::vector<std::uint32_t> children;
  BlockKind kind;
  ovt::fb::Rect rect;
  std::uint8_t level;
  std::int16_t qp;
  std::uint8_t intra_mode;  // CU: IntraPredModeC, PU: IntraPredModeY
};

// Builds a nested buffer the way §6.3.3 describes: its own builder, Finish() without identifier.
template <class BuildFn>
flatbuffers::Offset<flatbuffers::Vector<std::uint8_t>> nested(flatbuffers::FlatBufferBuilder& outer, BuildFn build) {
  flatbuffers::FlatBufferBuilder inner;
  inner.Finish(build(inner));
  outer.ForceVectorAlignment(inner.GetSize(), sizeof(std::uint8_t), inner.GetBufferMinAlignment());
  return outer.CreateVector(inner.GetBufferPointer(), inner.GetSize());
}

// §6.10: root 0, CTU 1, then {CU, PU, TU} = {2,3,4}, {5,6,7}, {8,9,10}, {11,12,13}.
std::vector<BlockSpec> section_6_10_blocks() {
  std::vector<BlockSpec> blocks{
      {0, -1, {1}, BlockKind::Other, {0, 0, 16, 16}, 0, 0, 0},
      {1, 0, {2, 5, 8, 11}, BlockKind::CTU, {0, 0, 16, 16}, 1, 32, 0},
  };
  const std::array<std::pair<int, int>, 4> origins{{{0, 0}, {8, 0}, {0, 8}, {8, 8}}};
  const std::array<std::uint8_t, 4> modes{1, 26, 0, 26};
  const std::array<std::int16_t, 4> qps{32, 32, 30, 33};
  for (std::uint32_t i = 0; i < 4; ++i) {
    const std::uint32_t cu = 2 + (3 * i);
    const auto [x, y] = origins.at(i);
    const ovt::fb::Rect rect{x, y, 8, 8};
    blocks.push_back({cu, 1, {cu + 1, cu + 2}, BlockKind::CU, rect, 2, qps.at(i), modes.at(i)});
    blocks.push_back({cu + 1, static_cast<std::int32_t>(cu), {}, BlockKind::PU, rect, 3, qps.at(i), modes.at(i)});
    blocks.push_back({cu + 2, static_cast<std::int32_t>(cu), {}, BlockKind::TU, rect, 3, qps.at(i), 0});
  }
  return blocks;
}

const std::vector<std::uint32_t> kSection610BlockIds = {2, 2, 5, 5, 2, 2, 5, 5, 8, 8, 11, 11, 8, 8, 11, 11};

// A finished FrameAnalysis with @p specs and a cells_w × cells_h grid whose block_id is @p block_ids.
std::vector<std::uint8_t> build_frame(const std::vector<BlockSpec>& specs, std::uint32_t cells_w, std::uint32_t cells_h,
                                      const std::vector<std::uint32_t>& block_ids) {
  flatbuffers::FlatBufferBuilder fbb;
  std::vector<flatbuffers::Offset<ovt::fb::Block>> blocks;
  for (const BlockSpec& s : specs) {
    flatbuffers::Offset<flatbuffers::Vector<std::uint8_t>> ext;
    const char* ext_type = "";
    if (s.kind == BlockKind::CU) {
      ext_type = "HevcCu";
      ext = nested(fbb, [&](flatbuffers::FlatBufferBuilder& b) {
        const auto modes_c = b.CreateVector(std::vector<std::uint8_t>{s.intra_mode});
        ovt::fb::HevcCuBuilder cu(b);
        cu.add_CuPredMode(ovt::fb::HevcPredMode::MODE_INTRA);
        cu.add_part_mode(ovt::fb::HevcPartMode::PART_2Nx2N);
        cu.add_QpY(static_cast<std::int8_t>(s.qp));
        cu.add_IntraPredModeC(modes_c);
        return cu.Finish();
      });
    } else if (s.kind == BlockKind::PU) {
      ext_type = "HevcPu";
      ext = nested(fbb, [&](flatbuffers::FlatBufferBuilder& b) {
        ovt::fb::HevcPuBuilder pu(b);
        pu.add_IntraPredModeY(s.intra_mode);
        return pu.Finish();
      });
    } else if (s.kind == BlockKind::CTU) {
      ext_type = "HevcCtu";
      ext = nested(fbb, [&](flatbuffers::FlatBufferBuilder& b) {
        ovt::fb::HevcCtuBuilder ctu(b);
        return ctu.Finish();
      });
    }
    const auto children = fbb.CreateVector(s.children);
    const auto type = fbb.CreateString(ext_type);
    ovt::fb::BlockBuilder b(fbb);
    b.add_id(s.id);
    b.add_parent(s.parent);
    b.add_children(children);
    b.add_kind(s.kind);
    b.add_rect(&s.rect);
    b.add_level(s.level);
    b.add_pred(ovt::fb::PredKind::Intra);
    b.add_qp(s.qp);
    b.add_ext_type(type);
    if (!ext.IsNull()) {
      b.add_ext(ext);
    }
    blocks.push_back(b.Finish());
  }
  const auto grid = ovt::fb::CreateCellGridDirect(fbb, cells_w, cells_h, &block_ids);
  const auto block_vec = fbb.CreateVector(blocks);
  ovt::fb::FrameAnalysisBuilder fa(fbb);
  fa.add_schema_version(1);
  fa.add_grid(grid);
  fa.add_blocks(block_vec);
  ovt::fb::FinishFrameAnalysisBuffer(fbb, fa.Finish());
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

const ovt::fb::FrameAnalysis& verified(const std::vector<std::uint8_t>& bytes) {
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
  return *ovt::fb::GetFrameAnalysis(bytes.data());
}

std::vector<std::uint32_t> ids_of(const std::vector<const ovt::fb::Block*>& blocks) {
  std::vector<std::uint32_t> ids;
  ids.reserve(blocks.size());
  for (const ovt::fb::Block* b : blocks) {
    ids.push_back(b->id());
  }
  return ids;
}

}  // namespace

TEST_CASE("test_tree: leaf_at and ancestors on the §6.10 example", "[fast][model]") {
  const auto bytes = build_frame(section_6_10_blocks(), 4, 4, kSection610BlockIds);
  const ovt::model::TreeWalker walker{verified(bytes)};

  // WI-0.084 acceptance test 3.
  const ovt::fb::Block* cu = walker.leaf_at(9, 9, BlockKind::CU);
  REQUIRE(cu != nullptr);
  CHECK(cu->id() == 11);
  CHECK(ids_of(walker.ancestors(11)) == std::vector<std::uint32_t>{1, 0});

  // Below the grid leaf: PU and TU of that CU; above it: the CTU and the root.
  CHECK(walker.leaf_at(9, 9, BlockKind::PU)->id() == 12);
  CHECK(walker.leaf_at(9, 9, BlockKind::TU)->id() == 13);
  CHECK(walker.leaf_at(9, 9, BlockKind::CTU)->id() == 1);
  CHECK(walker.leaf_at(9, 9, BlockKind::Other)->id() == 0);
  CHECK(walker.leaf_at(0, 0, BlockKind::PU)->id() == 3);
  CHECK(walker.leaf_at(15, 0, BlockKind::CU)->id() == 5);
  CHECK(walker.leaf_at(3, 12, BlockKind::TU)->id() == 10);
  // A kind that is not on the path falls back to the grid leaf.
  CHECK(walker.leaf_at(9, 9, BlockKind::MB)->id() == 11);
  // Outside the grid.
  CHECK(walker.leaf_at(16, 0, BlockKind::CU) == nullptr);
  CHECK(walker.leaf_at(0, -1, BlockKind::CU) == nullptr);

  CHECK(ids_of(walker.ancestors(13)) == std::vector<std::uint32_t>{11, 1, 0});
  CHECK(walker.ancestors(0).empty());
  CHECK(walker.ancestors(14).empty());
  CHECK(walker.block(13) != nullptr);
  CHECK(walker.block(14) == nullptr);
}

TEST_CASE("test_tree: pre_order visits the §6.10 tree in pre-order with depths", "[fast][model]") {
  const auto bytes = build_frame(section_6_10_blocks(), 4, 4, kSection610BlockIds);
  const ovt::model::TreeWalker walker{verified(bytes)};
  std::vector<std::pair<std::uint32_t, int>> visited;
  walker.pre_order([&](const ovt::fb::Block& b, int depth) { visited.emplace_back(b.id(), depth); });
  const std::vector<std::pair<std::uint32_t, int>> expected{{0, 0}, {1, 1}, {2, 2}, {3, 3},  {4, 3},  {5, 2},  {6, 3},
                                                            {7, 3}, {8, 2}, {9, 3}, {10, 3}, {11, 2}, {12, 3}, {13, 3}};
  CHECK(visited == expected);
}

TEST_CASE("test_tree: malformed trees give partial walks, never loops", "[fast][model]") {
  // Block 1 lists itself, the root, an out-of-range id and block 2 twice; block 2 claims parent 3 (after it).
  const std::vector<BlockSpec> specs{
      {0, -1, {1, 1, 99}, BlockKind::Other, {0, 0, 8, 8}, 0, 0, 0},
      {1, 0, {1, 0, 99, 2, 2}, BlockKind::CU, {0, 0, 8, 8}, 1, 0, 0},
      {2, 3, {}, BlockKind::PU, {0, 0, 8, 8}, 2, 0, 0},
      {3, 2, {}, BlockKind::TU, {0, 0, 8, 8}, 2, 0, 0},
  };
  const auto bytes = build_frame(specs, 2, 2, {1, 1, 1, 7});
  const ovt::model::TreeWalker walker{verified(bytes)};
  std::vector<std::uint32_t> visited;
  walker.pre_order([&](const ovt::fb::Block& b, int) { visited.push_back(b.id()); });
  CHECK(visited == std::vector<std::uint32_t>{0, 1, 2});
  CHECK(walker.ancestors(2).empty());  // parent 3 > 2 breaks V-005: stop
  CHECK(ids_of(walker.ancestors(1)) == std::vector<std::uint32_t>{0});
  CHECK(walker.leaf_at(1, 1, BlockKind::PU)->id() == 2);
  CHECK(walker.leaf_at(5, 5, BlockKind::CU) == nullptr);  // grid names block 7, which does not exist

  // No blocks, no grid.
  flatbuffers::FlatBufferBuilder fbb;
  ovt::fb::FrameAnalysisBuilder fa(fbb);
  ovt::fb::FinishFrameAnalysisBuffer(fbb, fa.Finish());
  const ovt::model::TreeWalker empty{*ovt::fb::GetFrameAnalysis(fbb.GetBufferPointer())};
  int calls = 0;
  empty.pre_order([&](const ovt::fb::Block&, int) { ++calls; });
  CHECK(calls == 0);
  CHECK(empty.leaf_at(0, 0, BlockKind::CU) == nullptr);
  CHECK(empty.ancestors(0).empty());
}

TEST_CASE("test_tree: ext_as checks ext_type and verifies the nested buffer", "[fast][model]") {
  const auto bytes = build_frame(section_6_10_blocks(), 4, 4, kSection610BlockIds);
  const ovt::model::TreeWalker walker{verified(bytes)};

  const auto* cu = ovt::model::ext_as<ovt::fb::HevcCu>(*walker.block(11));
  REQUIRE(cu != nullptr);
  CHECK(cu->QpY() == 33);
  CHECK(cu->part_mode() == ovt::fb::HevcPartMode::PART_2Nx2N);
  REQUIRE(cu->IntraPredModeC() != nullptr);
  CHECK(cu->IntraPredModeC()->Get(0) == 26);

  const auto* pu = ovt::model::ext_as<ovt::fb::HevcPu>(*walker.block(3));
  REQUIRE(pu != nullptr);
  CHECK(pu->IntraPredModeY() == 1);
  CHECK(ovt::model::ext_as<ovt::fb::HevcCtu>(*walker.block(1)) != nullptr);

  // Wrong type, no extension, empty ext_type of the root.
  CHECK(ovt::model::ext_as<ovt::fb::HevcPu>(*walker.block(11)) == nullptr);
  CHECK(ovt::model::ext_as<ovt::fb::Av1Block>(*walker.block(11)) == nullptr);
  CHECK(ovt::model::ext_as<ovt::fb::HevcTu>(*walker.block(4)) == nullptr);
  CHECK(ovt::model::ext_as<ovt::fb::HevcCu>(*walker.block(0)) == nullptr);
}

TEST_CASE("test_tree: ext_as rejects a nested buffer that does not verify", "[fast][model]") {
  flatbuffers::FlatBufferBuilder fbb;
  const std::vector<std::uint8_t> garbage{0xFF, 0xFF, 0xFF, 0x7F, 0x01, 0x02, 0x03, 0x04};
  const auto type = fbb.CreateString("HevcCu");
  const auto ext = fbb.CreateVector(garbage);
  ovt::fb::BlockBuilder b(fbb);
  b.add_kind(BlockKind::CU);
  b.add_ext_type(type);
  b.add_ext(ext);
  fbb.Finish(b.Finish());
  const auto* block = flatbuffers::GetRoot<ovt::fb::Block>(fbb.GetBufferPointer());
  CHECK(ovt::model::ext_as<ovt::fb::HevcCu>(*block) == nullptr);

  // ext_type set but no ext bytes.
  flatbuffers::FlatBufferBuilder fbb2;
  const auto type2 = fbb2.CreateString("HevcCu");
  ovt::fb::BlockBuilder b2(fbb2);
  b2.add_ext_type(type2);
  fbb2.Finish(b2.Finish());
  CHECK(ovt::model::ext_as<ovt::fb::HevcCu>(*flatbuffers::GetRoot<ovt::fb::Block>(fbb2.GetBufferPointer())) == nullptr);
}
