// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.093 acceptance test 2: for each checked-in fixture of core/tests/golden/, read it with the generated
// API, verify it, rebuild it from the object API (UnPack -> Pack, ForceDefaults off) and compare the bytes and
// the SHA-256 with the file and its `.sha256` (rule V-032). Values are asserted against chapter 06 §6.10 and
// the fixture descriptions of WI-0.093. The builders themselves are compared with the files too, so a builder
// edit without a regenerated fixture fails here as well as in golden_fixtures_regen.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fixtures.hpp"
#include "ovt_model_generated.h"
#include "sha256.hpp"

namespace {

using ovt::golden::Bytes;
namespace fb = ovt::fb;

Bytes read_file(const std::string& name) {
  std::ifstream in(std::filesystem::path(OVT_GOLDEN_DIR) / name, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_text(const std::string& name) {
  const Bytes bytes = read_file(name);
  return {bytes.begin(), bytes.end()};
}

Bytes repack(const fb::FrameAnalysis& root, bool size_prefixed) {
  flatbuffers::FlatBufferBuilder builder;  // ForceDefaults stays false
  // Stack object: no raw owning pointer for the static analyzer to track.
  fb::FrameAnalysisT native;
  root.UnPackTo(&native);
  const auto offset = fb::FrameAnalysis::Pack(builder, &native);
  if (size_prefixed) {
    fb::FinishSizePrefixedFrameAnalysisBuffer(builder, offset);
  } else {
    fb::FinishFrameAnalysisBuffer(builder, offset);
  }
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

}  // namespace

TEST_CASE("test_model_roundtrip: every fixture verifies and survives UnPack/Pack byte for byte",
          "[model][golden][fast]") {
  for (const auto& fixture : ovt::golden::kFixtures) {
    const std::string name = std::string(fixture.stem) + ".fa";
    INFO(name);
    const Bytes file = read_file(name);

    flatbuffers::Verifier verifier(file.data(), file.size());
    REQUIRE(fb::VerifyFrameAnalysisBuffer(verifier));
    REQUIRE(fb::FrameAnalysisBufferHasIdentifier(file.data()));
    const auto* root = fb::GetFrameAnalysis(file.data());
    CHECK(root->schema_version() == 1U);

    CHECK(repack(*root, false) == file);  // object-API round trip is byte-identical
    CHECK(fixture.build() == file);       // builder code and checked-in bytes agree

    const std::string expected_sha = ovt::golden::sha256_hex(file);
    std::string sha_line = expected_sha;
    sha_line += "  ";
    sha_line += name;
    sha_line += "\n";
    CHECK(read_text(name + ".sha256") == sha_line);

    // Size-prefixed form (the cache and stdout encoding, §6.1.1) verifies as stored.
    const Bytes prefixed = repack(*root, true);
    flatbuffers::Verifier prefixed_verifier(prefixed.data(), prefixed.size());
    CHECK(fb::VerifySizePrefixedFrameAnalysisBuffer(prefixed_verifier));
    CHECK(prefixed.size() >= file.size());  // the prefix may fit in alignment padding
  }
}

TEST_CASE("test_model_roundtrip: sha256 implementation matches the FIPS 180-4 vectors", "[model][golden][fast]") {
  const auto hex = [](const std::string& s) {
    return ovt::golden::sha256_hex({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
  };
  CHECK(hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

namespace {

template <class T>
std::vector<T> to_vector(const flatbuffers::Vector<T>* v) {
  return v == nullptr ? std::vector<T>{} : std::vector<T>(v->begin(), v->end());
}

Bytes load(std::string_view stem) {
  return read_file(std::string(stem) + ".fa");
}

// Rules the validator of WI-0.086 will check, asserted here on the fixtures independently (V-003, V-004,
// V-005, V-010, V-016, V-019, V-020, V-028).
void check_by_construction(const fb::FrameAnalysis& fa) {
  const auto* grid = fa.grid();
  const std::size_t cells = std::size_t{grid->cells_w()} * grid->cells_h();
  CHECK(grid->block_id()->size() == cells);
  CHECK(grid->qp()->size() == cells);
  CHECK(fa.qp_hist()->size() == 256U);

  const auto* blocks = fa.blocks();
  REQUIRE(blocks->size() > 1U);
  CHECK((*blocks)[0]->parent() == -1);
  for (std::uint32_t i = 0; i < blocks->size(); ++i) {
    const auto* b = (*blocks)[i];
    CHECK(b->id() == i);
    if (i > 0) {
      REQUIRE(b->parent() >= 0);
      REQUIRE(static_cast<std::uint32_t>(b->parent()) < i);
      const auto siblings = to_vector((*blocks)[static_cast<std::uint32_t>(b->parent())]->children());
      CHECK(std::count(siblings.begin(), siblings.end(), i) == 1);
    }
    std::uint64_t child_bits = 0;
    bool all_known = b->children() != nullptr;
    for (const auto child : to_vector(b->children())) {
      child_bits += (*blocks)[child]->bits_x8();
      all_known = all_known && (*blocks)[child]->bits_x8() != 0;
    }
    if (b->children() != nullptr && b->bits_x8() != 0 && all_known) {
      CHECK(b->bits_x8() >= child_bits);  // V-019
    }
  }

  std::map<std::uint32_t, std::uint64_t> cell_bits;
  for (std::size_t i = 0; i < cells; ++i) {
    const auto id = (*grid->block_id())[static_cast<flatbuffers::uoffset_t>(i)];
    CHECK(id != 0U);
    CHECK(id < blocks->size());
    cell_bits[id] += (*grid->bits_x8())[static_cast<flatbuffers::uoffset_t>(i)];
  }
  for (const auto& [id, sum] : cell_bits) {
    CHECK(sum == (*blocks)[id]->bits_x8());  // V-020
  }

  std::uint64_t hist_total = 0;
  for (const auto count : to_vector(fa.qp_hist())) {
    hist_total += count;
  }
  CHECK(hist_total == cells);  // V-028

  for (flatbuffers::uoffset_t k = 0; fa.refs() != nullptr && k < fa.refs()->size(); ++k) {
    const auto* ref = (*fa.refs())[k];
    std::uint32_t used = 0;
    for (const auto* list : {grid->ref0(), grid->ref1()}) {
      used += static_cast<std::uint32_t>(
          list == nullptr ? 0 : std::count(list->begin(), list->end(), static_cast<std::int8_t>(ref->index())));
    }
    CHECK(ref->usage_cells() == used);  // V-016
  }
}

}  // namespace

TEST_CASE("test_model_roundtrip: tiny16_hevc.fa carries the values of chapter 06 §6.10", "[model][golden][fast]") {
  const Bytes file = load("tiny16_hevc");
  const auto* fa = fb::GetFrameAnalysis(file.data());
  check_by_construction(*fa);

  CHECK(std::string_view(fa->plugin_id()->c_str()) == "ovt-plugin-de265");
  CHECK(std::string_view(fa->plugin_version()->c_str()) == "1.0.0+de265-1.0.15");
  const auto* frame = fa->frame();
  CHECK(frame->size_bytes() == 143U);
  CHECK(frame->coded_size()->w() == 16);
  CHECK(std::string_view(frame->ref_state_type()->c_str()) == "HevcRps");

  const auto* grid = fa->grid();
  CHECK(to_vector(grid->qp()) ==
        std::vector<std::int16_t>{32, 32, 32, 32, 32, 32, 32, 32, 30, 30, 33, 33, 30, 30, 33, 33});
  CHECK(to_vector(grid->block_id()) == std::vector<std::uint32_t>{2, 2, 5, 5, 2, 2, 5, 5, 8, 8, 11, 11, 8, 8, 11, 11});
  CHECK(to_vector(grid->intra_mode_y()) ==
        std::vector<std::int16_t>{1, 1, 26, 26, 1, 1, 26, 26, 0, 0, 26, 26, 0, 0, 26, 26});
  CHECK(to_vector(grid->bits_x8()) ==
        std::vector<std::uint32_t>{80, 80, 72, 72, 80, 80, 72, 72, 64, 64, 44, 44, 64, 64, 44, 44});
  CHECK(to_vector(grid->deblock_bs_v()) == std::vector<std::uint8_t>{0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0});
  CHECK(to_vector(grid->deblock_bs_h()) == std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 0});

  const auto* blocks = fa->blocks();
  REQUIRE(blocks->size() == 14U);
  CHECK((*blocks)[0]->bits_x8() == 1048U);
  CHECK(to_vector((*blocks)[1]->children()) == std::vector<std::uint32_t>{2, 5, 8, 11});
  const auto* cu3 = (*blocks)[11];
  CHECK(cu3->kind() == fb::BlockKind::CU);
  CHECK(cu3->qp() == 33);
  const auto* cu3_ext = flatbuffers::GetRoot<fb::HevcCu>(cu3->ext()->data());
  CHECK(cu3_ext->qPY_PRED() == 31);  // (30 + 32 + 1) >> 1
  CHECK(cu3_ext->QpY() == 33);
  CHECK(cu3_ext->CuQpDeltaVal() == 2);

  const auto hist = to_vector(fa->qp_hist());
  CHECK(hist[158] == 4U);
  CHECK(hist[160] == 8U);
  CHECK(hist[161] == 4U);
  const auto* slice = (*fa->partitions())[0];
  CHECK(slice->qp() == 32);
  CHECK(slice->header()->children()->size() == 8U);
}

TEST_CASE("test_model_roundtrip: tiny32_h264.fa has the four macroblock types of WI-0.093 step 2",
          "[model][golden][fast]") {
  const Bytes file = load("tiny32_h264");
  const auto* fa = fb::GetFrameAnalysis(file.data());
  check_by_construction(*fa);

  const auto* blocks = fa->blocks();
  REQUIRE(blocks->size() == 9U);
  CHECK(static_cast<fb::AvcMbType>((*blocks)[1]->partition_type()) == fb::AvcMbType::I_16x16_2_0_0);
  CHECK(static_cast<fb::AvcMbType>((*blocks)[2]->partition_type()) == fb::AvcMbType::P_L0_16x16);
  CHECK(static_cast<fb::AvcMbType>((*blocks)[3]->partition_type()) == fb::AvcMbType::P_8x8);
  CHECK(static_cast<fb::AvcMbType>((*blocks)[8]->partition_type()) == fb::AvcMbType::P_Skip);
  CHECK(to_vector((*blocks)[3]->children()) == std::vector<std::uint32_t>{4, 5, 6, 7});
  const auto* mb2 = flatbuffers::GetRoot<fb::AvcMb>((*blocks)[3]->ext()->data());
  CHECK(to_vector(mb2->sub_mb_type()).size() == 4U);

  // P_L0_16x16 mv (3, -1) quarter-pel -> neutral (12, -4) in every cell of MB1 (cell x 4..7, y 0..3).
  const auto* grid = fa->grid();
  for (std::uint32_t y = 0; y < 4; ++y) {
    for (std::uint32_t x = 4; x < 8; ++x) {
      CHECK((*grid->mv0_x())[(y * 8) + x] == 12);
      CHECK((*grid->mv0_y())[(y * 8) + x] == -4);
    }
  }
  REQUIRE(fa->refs()->size() == 2U);
  CHECK(std::string_view((*fa->refs())[0]->name()->c_str()) == "L0[0]");
  CHECK(std::string_view((*fa->refs())[1]->name()->c_str()) == "L0[1]");
}

TEST_CASE("test_model_roundtrip: tiny64_av1.fa has the four blocks of WI-0.093 step 3", "[model][golden][fast]") {
  const Bytes file = load("tiny64_av1");
  const auto* fa = fb::GetFrameAnalysis(file.data());
  check_by_construction(*fa);

  const auto* blocks = fa->blocks();
  REQUIRE(blocks->size() == 10U);
  const auto* sb = flatbuffers::GetRoot<fb::Av1Block>((*blocks)[1]->ext()->data());
  CHECK(sb->partition() == fb::Av1Partition::PARTITION_SPLIT);
  CHECK(sb->is_superblock() == 1);

  const auto block = [&](std::uint32_t id) { return flatbuffers::GetRoot<fb::Av1Block>((*blocks)[id]->ext()->data()); };
  CHECK(block(2)->YMode() == fb::Av1YMode::DC_PRED);
  const auto* newmv = block(4);
  CHECK(newmv->YMode() == fb::Av1YMode::NEWMV);
  CHECK(to_vector(newmv->Mv())[0] == -5);
  CHECK(to_vector(newmv->Mv())[1] == 7);
  CHECK(block(6)->YMode() == fb::Av1YMode::GLOBALMV);
  CHECK(block(6)->is_global() == 1);
  const auto* compound = block(8);
  CHECK(compound->YMode() == fb::Av1YMode::NEAREST_NEARESTMV);
  CHECK(to_vector(compound->RefFrame()) == std::vector<std::int8_t>{1, 7});

  // NEWMV Mv (-5, 7) in 1/8-pel -> neutral (-10, 14) as (row, col): cells of block 4 (x4 8..15, y4 0..7).
  const auto* grid = fa->grid();
  CHECK((*grid->mv0_y())[8] == -10);
  CHECK((*grid->mv0_x())[8] == 14);
  // The compound block (x4 8..15, y4 8..15) fills ref0 = LAST and ref1 = ALTREF and mv1 (V-016 counts both).
  const std::size_t compound_cell = (8U * 16U) + 8U;
  CHECK((*grid->ref0())[compound_cell] == 1);
  CHECK((*grid->ref1())[compound_cell] == 7);
  CHECK((*fa->refs())[0]->usage_cells() == 0U);
  CHECK((*fa->refs())[7]->usage_cells() == 64U);

  const auto filter = to_vector(fa->filter_params());
  const auto* params = flatbuffers::GetRoot<fb::Av1FilterParams>(filter.data());
  CHECK(to_vector(params->cdef()->cdef_idx()) == std::vector<std::int8_t>{2});
  CHECK(params->lr_units()->size() == 3U);
}

TEST_CASE("test_model_roundtrip: auxiliary fixture files are the generator's output", "[model][golden][fast]") {
  CHECK(read_file("tiny16.yuv") == ovt::golden::build_tiny16_yuv());
  CHECK(read_file("tiny16.yuv").size() == 384U);
  CHECK(read_text("tiny64_av1.inspect.json") == ovt::golden::build_tiny64_av1_inspect_json(read_file("tiny64_av1.fa")));
}
