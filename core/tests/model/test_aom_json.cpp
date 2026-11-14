// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.090: the libaom `inspect`-compatible JSON exporter (chapter 06 §6.8.3). Acceptance test 1 compares the
// export of tiny64_av1.fa with core/tests/golden/tiny64_av1.inspect.json (written by gen_fixtures from the
// §6.8.3 rules, WI-0.093) and its per-frame key set with aom_inspect_keys.txt. Acceptance test 2 (the
// aomanalyzer smoke script) is the CTest test_aom_json_smoke, registered next to this executable.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ovt/cxx/model/aom_inspect_json.hpp"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
using Bytes = std::vector<std::uint8_t>;

std::string read_text(const std::string& name) {
  std::ifstream in(std::filesystem::path(OVT_GOLDEN_DIR) / name, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Bytes read_bytes(const std::string& name) {
  const std::string text = read_text(name);
  return {text.begin(), text.end()};
}

const fb::FrameAnalysis* root_of(const Bytes& bytes) {
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  REQUIRE(fb::VerifyFrameAnalysisBuffer(verifier));
  return fb::GetFrameAnalysis(bytes.data());
}

std::string export_json(std::vector<const fb::FrameAnalysis*> frames, const ovt::model::AomJsonOptions& options) {
  std::ostringstream out;
  ovt::model::write_aom_inspect_json(out, frames, options);
  return out.str();
}

ovt::model::AomJsonOptions golden_options() {
  ovt::model::AomJsonOptions options;
  options.ovt_version = "0.0.0";  // the golden is version-independent
  return options;
}

std::vector<std::string> key_file() {
  std::istringstream in(read_text("aom_inspect_keys.txt"));
  std::vector<std::string> keys;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty()) {
      keys.push_back(line);
    }
  }
  return keys;
}

std::vector<std::string> keys_of(const nlohmann::ordered_json& frame) {
  std::vector<std::string> keys;
  for (const auto& item : frame.items()) {
    keys.push_back(item.key());
  }
  return keys;
}

// tiny64_av1.fa with accounting added: two positions, three aggregated SymbolCost records.
Bytes with_symbols(const Bytes& fixture) {
  fb::FrameAnalysisT native;
  fb::GetFrameAnalysis(fixture.data())->UnPackTo(&native);
  native.symbol_names = {"read_partition", "read_skip", "read\"quoted\\name"};
  const auto add = [&](std::uint16_t x4, std::uint16_t y4, std::uint16_t id, std::uint32_t bits, std::uint32_t count) {
    auto cost = std::make_unique<fb::SymbolCostT>();
    cost->x4 = x4;
    cost->y4 = y4;
    cost->symbol_id = id;
    cost->bits_x8 = bits;
    cost->count = count;
    native.symbols.push_back(std::move(cost));
  };
  add(0, 0, 0, 24, 1);
  add(0, 0, 1, 9, 2);
  add(8, 0, 2, 40, 3);
  flatbuffers::FlatBufferBuilder builder;
  fb::FinishFrameAnalysisBuffer(builder, fb::FrameAnalysis::Pack(builder, &native));
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

}  // namespace

TEST_CASE("test_aom_json: tiny64_av1.fa exports byte for byte as tiny64_av1.inspect.json", "[model][golden][fast]") {
  const Bytes fixture = read_bytes("tiny64_av1.fa");
  const std::string json = export_json({root_of(fixture)}, golden_options());
  CHECK(json == read_text("tiny64_av1.inspect.json"));
  // Determinism (§0.7.5): a second export is identical.
  CHECK(export_json({root_of(fixture)}, golden_options()) == json);
}

TEST_CASE("test_aom_json: the per-frame key set is inspect -A's", "[model][golden][fast]") {
  const std::vector<std::string> expected = key_file();
  const std::set<std::string> forbidden{"sb", "mm", "cfl", "ibc", "plt", "uvp", "cdef"};
  for (const auto& key : expected) {
    CHECK_FALSE(forbidden.contains(key));
  }

  // SPEC-ASSUMPTION(#269): without symbols the accounting keys are absent (§6.8.3), so AT1 drops them.
  SECTION("tiny64_av1.fa has no accounting: every key but symbolsMap and symbols, in order") {
    const Bytes fixture = read_bytes("tiny64_av1.fa");
    const auto doc = nlohmann::ordered_json::parse(export_json({root_of(fixture)}, golden_options()));
    REQUIRE(doc.is_array());
    REQUIRE(doc.size() == 2);
    CHECK(doc.back().is_null());
    std::vector<std::string> without_accounting;
    for (const auto& key : expected) {
      if (key != "symbolsMap" && key != "symbols") {
        without_accounting.push_back(key);
      }
    }
    CHECK(keys_of(doc.front()) == without_accounting);
  }

  SECTION("with symbols: the full key set, and the accounting layer in inspect.c's shape") {
    const Bytes fixture = with_symbols(read_bytes("tiny64_av1.fa"));
    const std::string json = export_json({root_of(fixture)}, golden_options());
    const auto doc = nlohmann::ordered_json::parse(json);
    CHECK(keys_of(doc.front()) == expected);
    CHECK(json.find("  \"symbolsMap\": [\"read_partition\",\"read_skip\",\"read\\\"quoted\\\\name\"],\n") !=
          std::string::npos);
    CHECK(json.find("  \"symbols\": [\n    [0,0],[0,24,1],[1,9,2],[8,0],[2,40,3]],\n") != std::string::npos);
    CHECK(doc.front()["symbolsMap"][2] == "read\"quoted\\name");
  }
}

TEST_CASE("test_aom_json: layer values follow §6.8.3", "[model][fast]") {
  const Bytes fixture = read_bytes("tiny64_av1.fa");
  const auto doc = nlohmann::ordered_json::parse(export_json({root_of(fixture)}, golden_options()));
  const auto& frame = doc.front();
  // 64x64 = 16x16 MI; one row of the grid crosses the intra (cols 0..7) and the NEWMV block (cols 8..15).
  REQUIRE(frame["mode"].size() == 16);
  REQUIRE(frame["mode"][0].size() == 16);
  CHECK(frame["mode"][0][0] == 0);      // DC_PRED
  CHECK(frame["mode"][0][8] == 16);     // NEWMV
  CHECK(frame["mode"][8][0] == 15);     // GLOBALMV
  CHECK(frame["mode"][8][8] == 17);     // NEAREST_NEARESTMV
  CHECK(frame["uv_mode"][0][0] == 13);  // UV_CFL_PRED
  CHECK(frame["uv_mode"][0][8] == 14);  // UV_MODE_INVALID for inter blocks
  // Mv (-5, 7) in 1/8 sample, row first in the model, col first in inspect.c.
  CHECK(frame["motionVectors"][0][8] == nlohmann::json::array({7, -5, 0, 0}));
  CHECK(frame["referenceFrame"][8][8] == nlohmann::json::array({1, 7}));
  CHECK(frame["referenceFrame"][0][0] == nlohmann::json::array({0, -1}));
  CHECK(frame["blockSize"][15][15] == 9);  // BLOCK_32X32
  CHECK(frame["frame"] == 0);
  CHECK(frame["config"] == nlohmann::json{{"MI_SIZE", 4}});
}

TEST_CASE("test_aom_json: framing, ordinals and configString over several records", "[model][fast]") {
  const Bytes fixture = read_bytes("tiny64_av1.fa");
  const auto* fa = root_of(fixture);
  ovt::model::AomJsonOptions options;
  options.decode_index_offset = 7;
  const std::string json = export_json({fa, nullptr, fa}, options);
  CHECK(json.rfind("[\n{\n", 0) == 0);
  CHECK(json.size() > 12);
  CHECK(json.substr(json.size() - 10) == "},\nnull\n]\n");
  const auto doc = nlohmann::ordered_json::parse(json);
  REQUIRE(doc.size() == 3);  // the null pointer is skipped
  CHECK(doc[0]["frame"] == 0);
  CHECK(doc[1]["frame"] == 1);
  CHECK(doc[2].is_null());
  const std::string expected_config = std::string("ovt ") + OVT_TEST_PRODUCT_VERSION + "; plugin " +
                                      fa->plugin_id()->str() + " " + fa->plugin_version()->str() +
                                      "; decode_index_offset 7";
  CHECK(doc[0]["configString"] == expected_config);

  CHECK(export_json({}, options) == "[\nnull\n]\n");
}

TEST_CASE("test_aom_json: records that are not AV1 are rejected", "[model][fast]") {
  const Bytes hevc = read_bytes("tiny16_hevc.fa");
  std::ostringstream out;
  const std::vector<const fb::FrameAnalysis*> frames{root_of(hevc)};
  CHECK_THROWS_AS(ovt::model::write_aom_inspect_json(out, frames), std::invalid_argument);
}
