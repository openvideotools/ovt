// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.089a: the YUView statistics CSV exporter (chapter 06 §6.8.2). Acceptance test 1 compares the export of
// tiny16_hevc.fa with core/tests/golden/tiny16_hevc.csv. The other cases cover the rules of §6.8.2 on small
// synthetic frames. The importer and acceptance test 2 follow in WI-0.089b.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ovt/cxx/model/yuview_csv.hpp"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
using Bytes = std::vector<std::uint8_t>;

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_text(const std::filesystem::path& path) {
  const Bytes bytes = read_file(path);
  return {bytes.begin(), bytes.end()};
}

Bytes stream_index(fb::Codec codec) {
  fb::StreamIndexT index;
  index.codec = codec;
  index.schema_version = 1;
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(fb::StreamIndex::Pack(builder, &index));
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

std::string export_csv(const std::vector<const fb::FrameAnalysis*>& frames, fb::Codec codec,
                       const ovt::model::YuviewOptions& options) {
  const Bytes index = stream_index(codec);
  std::ostringstream out;
  ovt::model::write_yuview_csv(out, *flatbuffers::GetRoot<fb::StreamIndex>(index.data()), frames, options);
  return out.str();
}

const fb::FrameAnalysis* verified(const Bytes& buffer) {
  flatbuffers::Verifier verifier(buffer.data(), buffer.size());
  REQUIRE(fb::VerifyFrameAnalysisBuffer(verifier));
  return fb::GetFrameAnalysis(buffer.data());
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  return lines;
}

bool has_line(const std::string& text, const std::string& line) {
  return text.find("\n" + line + "\n") != std::string::npos || text.starts_with(line + "\n");
}

// A 16x16 frame with one 16x16 leaf MB (or CU) per call to add_block; enough for the synthetic cases.
struct Synthetic {
  fb::FrameAnalysisT fa;

  explicit Synthetic(int display_index, std::int64_t poc = 0) {
    fa.schema_version = 1;
    fa.plugin_id = "test";
    auto info = std::make_unique<fb::FrameInfoT>();
    info->id = std::make_unique<fb::FrameId>(display_index, display_index);
    info->shown = true;
    info->poc = poc;
    info->coded_size = std::make_unique<fb::Rect>(0, 0, 16, 16);
    fa.frame = std::move(info);
    fa.grid = std::make_unique<fb::CellGridT>();
    fa.grid->cells_w = 4;
    fa.grid->cells_h = 4;
  }

  fb::BlockT& add_block(fb::BlockKind kind, fb::Rect rect) {
    auto block = std::make_unique<fb::BlockT>();
    block->id = static_cast<std::uint32_t>(fa.blocks.size());
    block->kind = kind;
    block->rect = std::make_unique<fb::Rect>(rect);
    block->partition_type = 255;
    block->level = 255;
    fa.blocks.push_back(std::move(block));
    return *fa.blocks.back();
  }

  [[nodiscard]] Bytes finish() const {
    flatbuffers::FlatBufferBuilder builder;
    fb::FinishFrameAnalysisBuffer(builder, fb::FrameAnalysis::Pack(builder, &fa));
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
  }
};

const std::filesystem::path kGolden{OVT_GOLDEN_DIR};

}  // namespace

TEST_CASE("yuview_csv: tiny16_hevc.fa exports to the golden CSV (WI-0.089 AT1)", "[model][yuview][golden][fast]") {
  const Bytes fa_bytes = read_file(kGolden / "tiny16_hevc.fa");
  const std::vector<const fb::FrameAnalysis*> frames{verified(fa_bytes)};
  const ovt::model::YuviewOptions options{.basename = "tiny16", .fps = 25.0};

  const std::string first = export_csv(frames, fb::Codec::H265, options);
  const std::string second = export_csv(frames, fb::Codec::H265, options);
  CHECK(first == second);  // deterministic (§0.7.5)

  const std::string golden = read_text(kGolden / "tiny16_hevc.csv");
  if (first != golden) {
    std::ofstream(std::filesystem::path(OVT_TEST_OUTPUT_DIR) / "tiny16_hevc.actual.csv", std::ios::binary) << first;
  }
  CHECK(first == golden);

  // The §6.8.2 example lines are part of it. SPEC-ASSUMPTION(#233): the example's QP row of the CU at (8, 8)
  // reads 34, but the fixture's QpY there is 33 (WI-0.093 AT2); the fixture wins.
  CHECK(golden.starts_with("%;syntax-version;v1.2\n%;seq-specs;tiny16;0;16;16;25\n%;type;0;OVT_PredMode;map\n"));
  for (const char* line : {"%;defaultRange;0;51;jet", "%;mapColor;26;0;128;255", "%;type;26;OVT_BlockGrid_CU;map",
                           "0;8;8;8;8;1;33", "0;0;8;8;8;2;0", "0;8;8;8;8;26;1"}) {
    CHECK(has_line(golden, line));
  }
}

TEST_CASE("yuview_csv: palette colours equal tools/yuview_palette.json", "[model][yuview][fast]") {
  const auto json =
      nlohmann::json::parse(read_text(std::filesystem::path(OVT_SOURCE_DIR) / "tools/yuview_palette.json"));
  const auto& colors = json.at("colors");
  REQUIRE(colors.size() == 35);

  // H.264 partition types 0..56 use every palette entry (value mod 35).
  Synthetic s(0);
  s.add_block(fb::BlockKind::MB, {0, 0, 16, 16}).partition_type = 3;
  const Bytes buffer = s.finish();
  const std::string csv = export_csv({verified(buffer)}, fb::Codec::H264, {});
  for (int v = 0; v <= 56; ++v) {
    const auto& c = colors.at(static_cast<std::size_t>(v % 35));
    const std::string line = "%;mapColor;" + std::to_string(v) + ";" + std::to_string(c.at(0).get<int>()) + ";" +
                             std::to_string(c.at(1).get<int>()) + ";" + std::to_string(c.at(2).get<int>());
    INFO(line);
    CHECK(has_line(csv, line));
  }
}

TEST_CASE("yuview_csv: uniform rule, sentinels and per-cell types", "[model][yuview][fast]") {
  Synthetic s(0);
  auto& mb = s.add_block(fb::BlockKind::MB, {0, 0, 16, 16});
  mb.pred = fb::PredKind::Intra;
  mb.qp = -1;  // sentinel: no OVT_QP row
  auto& grid = *s.fa.grid;
  grid.intra_mode_y.assign(16, 2);
  grid.intra_mode_y[5] = 7;         // not uniform -> one row per cell
  grid.intra_mode_y[0] = -1;        // sentinel cell -> no row
  grid.intra_mode_c.assign(16, 1);  // uniform -> one block row
  grid.ref0.assign(16, -1);         // all sentinel -> type declared, no rows
  grid.tx_log2_w.assign(16, 255);
  grid.tx_log2_w[15] = 2;  // per-cell type: only the non-sentinel cell
  const Bytes buffer = s.finish();
  const std::string csv = export_csv({verified(buffer)}, fb::Codec::H264, {});

  const auto rows = lines_of(csv);
  const auto count = [&](const std::string& prefix_type) {
    return std::count_if(rows.begin(), rows.end(), [&](const std::string& l) {
      return !l.starts_with("%") && l.find(prefix_type) != std::string::npos;
    });
  };
  CHECK(count(";4;4;2;") == 15);  // 16 cells minus the sentinel cell
  CHECK(has_line(csv, "0;4;4;4;4;2;7"));
  CHECK_FALSE(has_line(csv, "0;0;0;4;4;2;-1"));
  CHECK(has_line(csv, "0;0;0;16;16;3;1"));
  CHECK(has_line(csv, "%;type;6;OVT_Ref0;map"));
  CHECK(has_line(csv, "%;mapColor;-1;0;0;0"));
  CHECK(count(";6;") == 0);
  CHECK(has_line(csv, "0;12;12;4;4;8;2"));
  CHECK(count(";16;16;1;") == 0);  // no QP row
  CHECK(has_line(csv, "0;0;0;16;16;0;1"));
  CHECK(has_line(csv, "%;type;23;OVT_BlockGrid_MB;map"));
  CHECK_FALSE(has_line(csv, "%;type;4;OVT_MV0;vector"));     // no vectors -> no type block
  CHECK_FALSE(has_line(csv, "%;type;9;OVT_TxLog2H;range"));  // empty vector -> no type block
}

TEST_CASE("yuview_csv: vectors, MVD sentinel, ordering and options", "[model][yuview][fast]") {
  Synthetic later(1, 9);
  later.add_block(fb::BlockKind::Block, {0, 0, 16, 16}).qp = 200;
  Synthetic hidden(2, 4);
  hidden.fa.frame->shown = false;
  Synthetic first(0, 8);
  first.add_block(fb::BlockKind::Block, {0, 0, 16, 16}).qp = 100;
  for (const auto& [list, x, mvd] : {std::tuple{0, 8, std::int16_t{-32768}}, std::tuple{0, 0, std::int16_t{4}},
                                     std::tuple{1, 0, std::int16_t{-2}}}) {
    auto mv = std::make_unique<fb::MotionVectorT>();
    mv->list = static_cast<std::uint8_t>(list);
    mv->rect = std::make_unique<fb::Rect>(x, 0, 8, 8);
    mv->mv_x = 12;
    mv->mv_y = -4;
    mv->mvd_x = mvd;
    mv->mvd_y = mvd;
    first.fa.mvs.push_back(std::move(mv));
  }
  const Bytes b_later = later.finish();
  const Bytes b_hidden = hidden.finish();
  const Bytes b_first = first.finish();
  // Display order deliberately out of POC-column order: rows must still come out sorted.
  const std::vector<const fb::FrameAnalysis*> frames{verified(b_later), verified(b_hidden), nullptr, verified(b_first)};

  const std::string csv = export_csv(frames, fb::Codec::AV1, {.basename = "a;b", .fps = 30000.0 / 1001.0});
  CHECK(csv.starts_with("%;syntax-version;v1.2\n%;seq-specs;a_b;0;16;16;29.97003\n"));
  CHECK(has_line(csv, "%;defaultRange;0;255;jet"));
  CHECK(has_line(csv, "%;vectorColor;0;255;0;255"));
  CHECK(has_line(csv, "%;scaleToBlockSize;0"));
  CHECK(has_line(csv, "0;0;0;8;8;4;12;-4"));
  CHECK(has_line(csv, "0;8;0;8;8;4;12;-4"));
  CHECK(has_line(csv, "0;0;0;8;8;21;4;4"));
  CHECK_FALSE(has_line(csv, "0;8;0;8;8;21;-32768;-32768"));
  CHECK(has_line(csv, "0;0;0;8;8;22;-2;-2"));
  const auto pos_first = csv.find("\n0;0;0;16;16;1;100\n");
  const auto pos_later = csv.find("\n1;0;0;16;16;1;200\n");
  REQUIRE(pos_first != std::string::npos);
  REQUIRE(pos_later != std::string::npos);
  CHECK(pos_first < pos_later);
  CHECK(csv.find("\n2;") == std::string::npos);  // hidden frame skipped

  // POC column = FrameInfo.poc, a type subset, unknown fps.
  const std::string poc_csv = export_csv(frames, fb::Codec::AV1, {.types = {1}, .poc_column_is_poc = true});
  CHECK(poc_csv ==
        "%;syntax-version;v1.2\n%;seq-specs;;0;16;16;0\n%;type;1;OVT_QP;range\n"
        "%;defaultRange;0;255;jet\n8;0;0;16;16;1;100\n9;0;0;16;16;1;200\n");

  // No exportable frame: header only.
  CHECK(export_csv({}, fb::Codec::H265, {.fps = 25}) == "%;syntax-version;v1.2\n%;seq-specs;;0;0;0;25\n");
}
