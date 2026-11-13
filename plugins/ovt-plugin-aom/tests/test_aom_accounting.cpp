// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Tests of the accounting mapping of ovt-plugin-aom (WI-2.157, chapter 07 §7.6.1 accounting row): map_accounting()
// on hand-made trees, and the plugin's symbols and bits_x8 for libaom test vectors.
// AT1: for every frame, Σ grid.bits_x8 == Σ leaf Block.bits_x8 exactly (and the validator's V-019/V-020/V-024 hold);
//      frames with several tile groups (av1-1-b8-22-svc-L1T2) carry the "accounting covers last tile group only" note
//      and no grid.bits_x8 instead (§7.6.1).
// AT2 (SPEC-ASSUMPTION(#408): inspect -a omits the first record at each context): the plugin's SymbolCost per (x4, y4,
// id) equals the totals of libaom's accounting records, read here through
//      libaom's inspection API (the same callback examples/inspect uses); those records, printed with inspect.c's
//      rule, equal inspect -a's `symbols` arrays recorded in data/accounting_reference.json
//      (data/make_accounting_reference.py).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <flatbuffers/flatbuffers.h>

#include <nlohmann/json.hpp>

// libaom, for the oracle decoder (tp::aom_inspection: public API plus the semi-public inspection headers).
#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

#include "av1/decoder/accounting.h"
#include "av1/decoder/inspection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <regex>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "aom_test_util.inc"
#include "block_data.hpp"
#include "map_accounting.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace fb = ovt::fb;
using ovt::aom::AccountingData;
using ovt::aom::map_accounting;

namespace {

// ---- map_accounting() on a hand-made tree ----------------------------------------------------------------------
//
// A 4 x 2 cell frame: root 0 -> SB 1 -> leaves 2 (cells x 0..1) and 4 (cells x 2..3); leaf 2 has a TXB child 3.
// Pre-order ids, as map_blocks() emits them.
const std::vector<int32_t> kParent = {-1, 0, 1, 2, 1};
const std::vector<uint32_t> kBlockId = {2, 2, 4, 4,  //
                                        2, 2, 4, 4};
constexpr uint32_t kW = 4;
constexpr uint32_t kH = 2;

AccountingData hand_made() {
  AccountingData a;
  a.valid = true;
  a.names = {"partition", "skip", "lr_unit"};
  a.symbols = {
      {-1, -1, 2, 40, 2},  // read before the tile's first block sets a context: frame level
      {0, 0, 0, 16, 1},    // leaf 2
      {0, 0, 1, 7, 1},     // leaf 2
      {2, 0, 0, 10, 1},    // leaf 4
      {0, 0, 0, 5, 1},     // leaf 2 again, not adjacent to the first (0, 0, 0) record: aggregated with it
      {4, 1, 1, 9, 3},     // a block in libaom's padding column (x4 == cells_w): frame level
      {3, 1, 2, 3, 1},     // inside leaf 4 but not at its origin (libaom's context of the previous block)
  };
  return a;
}

}  // namespace

TEST_CASE("aom accounting: bits per block, cells and symbols of a hand-made frame", "[fast]") {
  const auto m = map_accounting(hand_made(), kParent, kBlockId, kW, kH);
  // Leaf 2: 16 + 7 + 5 = 28; leaf 4: 10 + 3 = 13; the TXB has none (0 = unknown); SB 1 = 41; root = 41 + 40 + 9.
  CHECK(m.block_bits == std::vector<uint32_t>{90, 41, 28, 0, 13});
  CHECK(m.frame_level_bits == 49);
  // Floor plus remainder in raster order (§6.3.4): 28 / 4 = 7 r 0; 13 / 4 = 3 r 1 -> the first cell gets +1.
  CHECK(m.cell_bits == std::vector<uint32_t>{7, 7, 4, 3,  //
                                             7, 7, 3, 3});
  // AT1 on the hand-made frame.
  CHECK(std::accumulate(m.cell_bits.begin(), m.cell_bits.end(), uint64_t{0}) == 28 + 13);
  REQUIRE(m.symbols.size() == 4);  // (y4, x4, id) order; frame-level records are left out (V-024)
  auto key = [](const ovt::aom::SymbolTotal& s) {
    return std::make_tuple(s.y4, s.x4, s.symbol_id, s.bits_x8, s.count);
  };
  CHECK(key(m.symbols[0]) == std::make_tuple(uint16_t{0}, uint16_t{0}, uint16_t{0}, uint32_t{21}, uint32_t{2}));
  CHECK(key(m.symbols[1]) == std::make_tuple(uint16_t{0}, uint16_t{0}, uint16_t{1}, uint32_t{7}, uint32_t{1}));
  CHECK(key(m.symbols[2]) == std::make_tuple(uint16_t{0}, uint16_t{2}, uint16_t{0}, uint32_t{10}, uint32_t{1}));
  CHECK(key(m.symbols[3]) == std::make_tuple(uint16_t{1}, uint16_t{3}, uint16_t{2}, uint32_t{3}, uint32_t{1}));
  CHECK(m.names == hand_made().names);
  CHECK_FALSE(m.last_tile_group_only);
}

TEST_CASE("aom accounting: several tile groups keep block bits but drop the cell bits", "[fast]") {
  AccountingData a = hand_made();
  a.tile_groups = 2;
  const auto m = map_accounting(a, kParent, kBlockId, kW, kH);
  CHECK(m.last_tile_group_only);
  CHECK(m.cell_bits.empty());  // grid.bits_x8 announced absent for the frame (§7.6.1)
  CHECK(m.block_bits[0] == 90);
  CHECK(m.symbols.size() == 4);
}

TEST_CASE("aom accounting: no accounting gives zero bits and no symbols", "[fast]") {
  const auto m = map_accounting(AccountingData{}, kParent, kBlockId, kW, kH);
  CHECK(m.block_bits == std::vector<uint32_t>(kParent.size(), 0));
  CHECK(m.cell_bits.empty());
  CHECK(m.symbols.empty());
  CHECK(m.names.empty());
}

TEST_CASE("aom accounting: a cell without a leaf charges the root and gets no bits", "[fast]") {
  const std::vector<uint32_t> ids = {2, 2, 4, 0,  //
                                     2, 2, 4, 4};
  AccountingData a;
  a.valid = true;
  a.names = {"skip"};
  a.symbols = {{3, 0, 0, 11, 1}, {2, 1, 0, 6, 1}};
  const auto m = map_accounting(a, kParent, ids, kW, kH);
  CHECK(m.block_bits == std::vector<uint32_t>{17, 6, 0, 0, 6});
  CHECK(m.cell_bits == std::vector<uint32_t>{0, 0, 2, 0,  //
                                             0, 0, 2, 2});
  CHECK(m.symbols.size() == 2);  // the record is still in the grid, so it stays a SymbolCost
}

// ---- the plugin on libaom test vectors --------------------------------------------------------------------------

namespace {

// The validator findings a plugin buffer has until the host fills FrameInfo timing and the tiles' unit indices
// (§7.5.3 A6), and V-006 on the root rect of a coded size that is not a multiple of 4 (spec question #360); the same
// waivers as test_aom_map_mi.cpp.
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

// Pushes the stream like the host (one OBU per call, hints from the frame OBUs) and keeps every FrameAnalysis.
std::vector<std::vector<uint8_t>> analyse(const std::vector<ovt::aom::test::TemporalUnit>& tus, size_t max_frames) {
  ovt::aom::test::Instance inst;
  REQUIRE(inst.status == OVT_OK);
  std::vector<std::vector<uint8_t>> out;
  int32_t next = 0;
  auto drain = [&] {
    for (;;) {
      ovt_Buffer buf{};
      REQUIRE(inst.api.pull(inst.dec, 0, &buf) == OVT_OK);
      if (buf.data == nullptr) {
        return;
      }
      const std::span<const uint8_t> s(buf.data, buf.size);
      out.emplace_back(s.begin(), s.end());
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
    if (out.size() >= max_frames) {
      break;
    }
  }
  REQUIRE(inst.api.flush(inst.dec) == OVT_OK);
  drain();
  if (out.size() > max_frames) {
    out.resize(max_frames);
  }
  return out;
}

// ---- oracle: libaom's accounting records, as examples/inspect reads them ----------------------------------------

struct RawRecord {
  int x;
  int y;
  uint32_t id;
  uint32_t bits;
  uint32_t samples;
};
struct RawFrame {
  std::vector<std::string> names;
  std::vector<RawRecord> records;
};
struct RawRun {
  insp_frame_data fd{};
  std::vector<RawFrame> frames;
};

void on_raw_frame(void* pbi, void* data) {
  auto* run = static_cast<RawRun*>(data);
  ifd_inspect(&run->fd, pbi, 0);
  const Accounting* acc = run->fd.accounting;
  REQUIRE(acc != nullptr);
  RawFrame f;
  for (const char* s : std::span(acc->syms.dictionary.strs).first(static_cast<size_t>(acc->syms.dictionary.num_strs))) {
    f.names.emplace_back(s);
  }
  for (const AccountingSymbol& s : std::span(acc->syms.syms, static_cast<size_t>(acc->syms.num_syms))) {
    f.records.push_back(RawRecord{s.context.x, s.context.y, s.id, s.bits, s.samples});
  }
  run->frames.push_back(std::move(f));
}

// The inspect.c read_frame() loop over every temporal unit: one aom_codec_decode() call per frame, user_priv set.
std::vector<RawFrame> raw_accounting(const std::vector<ovt::aom::test::TemporalUnit>& tus, size_t max_frames) {
  RawRun run;
  aom_codec_ctx_t ctx{};
  aom_codec_dec_cfg_t cfg{};
  cfg.threads = 1;
  cfg.allow_lowbitdepth = 1;
  REQUIRE(aom_codec_dec_init(&ctx, aom_codec_av1_dx(), &cfg, 0) == AOM_CODEC_OK);
  ifd_init(&run.fd, 8, 8);
  aom_inspect_init ii{};
  ii.inspect_cb = &on_raw_frame;
  ii.inspect_ctx = &run;
  REQUIRE(aom_codec_control(&ctx, AV1_SET_INSPECTION_CALLBACK, &ii) == AOM_CODEC_OK);
  for (const auto& tu : tus) {
    std::vector<uint8_t> bytes;
    for (const auto& o : tu) {
      bytes.insert(bytes.end(), o.bytes.begin(), o.bytes.end());
    }
    const uint8_t* p = bytes.data();
    size_t len = bytes.size();
    while (len > 0) {
      Av1DecodeReturn ret{};
      REQUIRE(aom_codec_decode(&ctx, p, len, &ret) == AOM_CODEC_OK);
      const auto consumed = static_cast<size_t>(ret.buf - p);
      if (consumed == 0) {
        break;
      }
      p = ret.buf;
      len -= consumed;
      aom_codec_iter_t iter = nullptr;
      while (aom_codec_get_frame(&ctx, &iter) != nullptr) {
      }
    }
    if (run.frames.size() >= max_frames) {
      break;
    }
  }
  aom_codec_destroy(&ctx);
  ifd_clear(&run.fd);
  if (run.frames.size() > max_frames) {
    run.frames.resize(max_frames);
  }
  return run.frames;
}

// The `symbols` array inspect.c's put_accounting() prints for these records: a context marker [x, y] *in place of*
// the first record at each new context, then [id, bits, samples] for the following records of that context.
nlohmann::json as_inspect_prints(const std::vector<RawRecord>& records) {
  nlohmann::json out = nlohmann::json::array();
  int cx = -2;
  int cy = -2;
  for (const RawRecord& r : records) {
    if (r.x != cx || r.y != cy) {
      out.push_back({r.x, r.y});
    } else {
      out.push_back({r.id, r.bits, r.samples});
    }
    cx = r.x;
    cy = r.y;
  }
  return out;
}

const nlohmann::json& reference() {
  static const nlohmann::json j = [] {
    std::ifstream in(std::filesystem::path(OVT_AOM_TEST_DATA_DIR) / "accounting_reference.json");
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
  }();
  return j;
}

// Σ bits_x8 of the leaf blocks (those grid.block_id names) and Σ grid.bits_x8.
std::pair<uint64_t, uint64_t> leaf_and_cell_bits(const fb::FrameAnalysis& fa) {
  const fb::CellGrid& g = *fa.grid();
  std::vector<bool> leaf(fa.blocks()->size(), false);
  for (const uint32_t id : *g.block_id()) {
    leaf.at(id) = true;
  }
  uint64_t leaves = 0;
  for (uint32_t id = 0; id < fa.blocks()->size(); ++id) {
    leaves += leaf[id] ? fa.blocks()->Get(id)->bits_x8() : 0U;
  }
  uint64_t cells = 0;
  if (g.bits_x8() != nullptr) {
    for (const uint32_t b : *g.bits_x8()) {
      cells += b;
    }
  }
  return {leaves, cells};
}

}  // namespace

TEST_CASE("aom accounting AT1: sum of grid.bits_x8 equals sum of leaf Block.bits_x8 in every frame",
          "[fast][third_party][needs-corpus]") {
  // Odd sizes (padding MIs), intra, inter, IntraBC and palette, film grain, 10-bit, hidden frames, SVC.
  const auto id =
      GENERATE(as<std::string>{}, "aom_av1_1_b8_01_size_16x16", "aom_av1_1_b8_01_size_66x66",
               "aom_av1_1_b8_01_size_196x196", "aom_av1_1_b8_02_allintra", "aom_av1_1_b8_05_mv", "aom_av1_1_b8_06_mfmv",
               "aom_av1_1_b8_16_intra_only_intrabc_extreme_dv", "aom_av1_1_b8_22_svc_L1T2",
               "aom_av1_1_b8_23_film_grain_50", "aom_av1_1_b10_00_quantizer_10");
  INFO(id);
  const auto frames = analyse(ovt::aom::test::read_ivf(ovt::test::corpus_or_skip(id)), 12);
  REQUIRE(!frames.empty());
  for (size_t f = 0; f < frames.size(); ++f) {
    INFO("frame " << f);
    check_valid(frames[f]);
    const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(frames[f].data());
    size_t notes = 0;
    for (const fb::Diagnostic* d : *fa.diagnostics()) {
      if (d->message()->str() == "accounting covers last tile group only") {
        ++notes;
        CHECK(d->severity() == fb::Severity::Info);
        CHECK(d->code()->str() == "DEC-AV1-0002");
      }
    }
    const bool several_tile_groups = id == "aom_av1_1_b8_22_svc_L1T2";  // 3 OBU_TILE_GROUPs per frame
    CHECK(notes == (several_tile_groups ? 1U : 0U));
    if (several_tile_groups) {
      CHECK(fa.grid()->bits_x8() == nullptr);  // announced absent for the frame
    } else {
      REQUIRE(fa.grid()->bits_x8() != nullptr);
      REQUIRE(fa.grid()->bits_x8()->size() == fa.grid()->block_id()->size());
    }
    const auto [leaves, cells] = leaf_and_cell_bits(fa);
    CHECK((several_tile_groups || cells == leaves));
    CHECK(leaves > 0);
    // Σ symbols (in-grid contexts) == Σ leaf bits; the root adds the frame-level records on top.
    uint64_t symbols = 0;
    for (const fb::SymbolCost* s : *fa.symbols()) {
      symbols += s->bits_x8();
      CHECK(s->symbol_id() < fa.symbol_names()->size());
    }
    CHECK(symbols == leaves);
    CHECK(fa.blocks()->Get(0)->bits_x8() >= symbols);
  }
}

TEST_CASE("aom accounting AT2: SymbolCost per (x4, y4, id) equals libaom inspect's accounting",
          "[fast][third_party][needs-corpus]") {
  // 16x16: eight frames; 196x196: padding MIs (frame-level records); svc-L1T2: three tile groups per frame, where
  // libaom's accounting holds the last tile group only (the oracle sees the same).
  const auto id = GENERATE(as<std::string>{}, "aom_av1_1_b8_01_size_16x16", "aom_av1_1_b8_01_size_196x196",
                           "aom_av1_1_b8_22_svc_L1T2");
  INFO(id);
  const auto& ref = reference()["streams"][id];
  REQUIRE(!ref.empty());
  const auto tus = ovt::aom::test::read_ivf(ovt::test::corpus_or_skip(id));
  const auto frames = analyse(tus, ref.size());
  const auto raw = raw_accounting(tus, ref.size());
  REQUIRE(frames.size() == ref.size());
  REQUIRE(raw.size() == ref.size());
  size_t compared = 0;
  for (size_t f = 0; f < ref.size(); ++f) {
    INFO("frame " << f);
    // The oracle records are what inspect -a prints, symbol for symbol.
    CHECK(raw[f].names == ref[f]["symbolsMap"].get<std::vector<std::string>>());
    CHECK(as_inspect_prints(raw[f].records) == ref[f]["symbols"]);

    const fb::FrameAnalysis& fa = *fb::GetFrameAnalysis(frames[f].data());
    const uint32_t w = fa.grid()->cells_w();
    const uint32_t h = fa.grid()->cells_h();
    std::map<std::tuple<int, int, uint32_t>, std::pair<uint64_t, uint64_t>> want;  // (y, x, id) -> bits, count
    uint64_t frame_level = 0;
    for (const RawRecord& r : raw[f].records) {
      if (r.x < 0 || r.y < 0 || static_cast<uint32_t>(r.x) >= w || static_cast<uint32_t>(r.y) >= h) {
        frame_level += r.bits;
        continue;
      }
      auto& t = want[{r.y, r.x, r.id}];
      t.first += r.bits;
      t.second += r.samples;
    }
    REQUIRE(fa.symbols()->size() == want.size());
    size_t k = 0;
    for (const auto& [key, t] : want) {
      const fb::SymbolCost* s = fa.symbols()->Get(static_cast<uint32_t>(k++));
      INFO("(x4, y4, id) = (" << std::get<1>(key) << ", " << std::get<0>(key) << ", " << std::get<2>(key) << ")");
      CHECK(s->y4() == std::get<0>(key));
      CHECK(s->x4() == std::get<1>(key));
      CHECK(s->symbol_id() == std::get<2>(key));
      CHECK(s->bits_x8() == t.first);
      CHECK(s->count() == t.second);
    }
    compared += want.size();
    std::vector<std::string> names;
    for (const auto* n : *fa.symbol_names()) {
      names.push_back(n->str());
    }
    CHECK(names == raw[f].names);
    // The root carries its children's bits plus the frame-level records.
    const fb::Block* root = fa.blocks()->Get(0);
    uint64_t children = 0;
    for (const uint32_t c : *root->children()) {
      children += fa.blocks()->Get(c)->bits_x8();
    }
    CHECK(root->bits_x8() == children + frame_level);
  }
  CHECK(compared > 40);
}

TEST_CASE("aom accounting: the same stream gives byte-identical symbols and bits",
          "[fast][third_party][needs-corpus]") {
  const auto tus = ovt::aom::test::read_ivf(ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv"));
  CHECK(analyse(tus, 4) == analyse(tus, 4));
}
