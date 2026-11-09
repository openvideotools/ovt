// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.089: the YUView statistics CSV exporter and importer (chapter 06 §6.8.2). Acceptance test 1 compares
// the export of tiny16_hevc.fa with core/tests/golden/tiny16_hevc.csv; acceptance test 2 imports that file
// and compares grid.qp and grid.intra_mode_y with the fixture. The other cases cover the rules of §6.8.2 on
// small synthetic frames and the importer's handling of bad input.

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

// Field by field rather than designated initializers: GCC's -Wmissing-field-initializers flags omitted members.
ovt::model::YuviewOptions make_options(std::string basename, double fps, std::vector<std::uint16_t> types = {},
                                       bool poc_column_is_poc = false) {
  ovt::model::YuviewOptions options;
  options.basename = std::move(basename);
  options.fps = fps;
  options.types = std::move(types);
  options.poc_column_is_poc = poc_column_is_poc;
  return options;
}

const fb::FrameAnalysis* verified(const Bytes& buffer) {
  flatbuffers::Verifier verifier(buffer.data(), buffer.size());
  REQUIRE(fb::VerifyFrameAnalysisBuffer(verifier));
  return fb::GetFrameAnalysis(buffer.data());
}

const fb::FrameAnalysis* import_csv(const std::string& text, int poc, Bytes& storage) {
  std::istringstream in(text);
  storage = ovt::model::read_yuview_csv(in, poc);
  return verified(storage);
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

template <typename T>
std::vector<T> to_vector(const flatbuffers::Vector<T>* v) {
  return v == nullptr ? std::vector<T>{} : std::vector<T>(v->begin(), v->end());
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
  const ovt::model::YuviewOptions options = make_options("tiny16", 25.0);

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

TEST_CASE("yuview_csv: the golden CSV imports back to the fixture's grid (WI-0.089 AT2)", "[model][yuview][fast]") {
  const Bytes fa_bytes = read_file(kGolden / "tiny16_hevc.fa");
  const auto* fixture = verified(fa_bytes);
  Bytes storage;
  const auto* imported = import_csv(read_text(kGolden / "tiny16_hevc.csv"), 0, storage);

  CHECK(imported->plugin_id()->str() == "yuview-csv-import");
  REQUIRE(imported->grid() != nullptr);
  CHECK(imported->grid()->cells_w() == 4);
  CHECK(imported->grid()->cells_h() == 4);
  CHECK(to_vector(imported->grid()->qp()) == to_vector(fixture->grid()->qp()));
  CHECK(to_vector(imported->grid()->intra_mode_y()) == to_vector(fixture->grid()->intra_mode_y()));
  CHECK(to_vector(imported->grid()->intra_mode_c()) == to_vector(fixture->grid()->intra_mode_c()));
  CHECK(to_vector(imported->grid()->pred()) == to_vector(fixture->grid()->pred()));
  CHECK(to_vector(imported->grid()->cbf()) == to_vector(fixture->grid()->cbf()));
  CHECK(to_vector(imported->grid()->deblock_bs_v()) == to_vector(fixture->grid()->deblock_bs_v()));
  CHECK(to_vector(imported->grid()->ref0()) == to_vector(fixture->grid()->ref0()));  // header only: all -1
  CHECK(imported->grid()->mv0_x() == nullptr);

  // OVT types without a grid field are reported once each (Info); nothing is malformed.
  REQUIRE(imported->diagnostics() != nullptr);
  for (const auto* d : *imported->diagnostics()) {
    CHECK(d->severity() == fb::Severity::Info);
    CHECK(d->code()->str() == "MDL-CSV-0003");
  }

  // Another POC selects no rows: the declared vectors are all sentinel.
  Bytes other;
  const auto* empty = import_csv(read_text(kGolden / "tiny16_hevc.csv"), 1, other);
  CHECK(to_vector(empty->grid()->qp()) == std::vector<std::int16_t>(16, -1));
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

  const std::string csv = export_csv(frames, fb::Codec::AV1, make_options("a;b", 30000.0 / 1001.0));
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
  const std::string poc_csv = export_csv(frames, fb::Codec::AV1, make_options("", 0.0, {1}, true));
  CHECK(poc_csv ==
        "%;syntax-version;v1.2\n%;seq-specs;;0;16;16;0\n%;type;1;OVT_QP;range\n"
        "%;defaultRange;0;255;jet\n8;0;0;16;16;1;100\n9;0;0;16;16;1;200\n");

  // No exportable frame: header only.
  CHECK(export_csv({}, fb::Codec::H265, make_options("", 25.0)) == "%;syntax-version;v1.2\n%;seq-specs;;0;0;0;25\n");
}

TEST_CASE("yuview_csv: importer maps vectors and reports bad input", "[model][yuview][fast]") {
  const std::string text =
      "%;syntax-version;v1.2\r\n"
      "%;seq-specs;x;0;8;8;25\n"
      "%;type;4;OVT_MV0;vector\n"
      "%;type;7;Foreign;range\n"
      "%;type;1;OVT_QP;range\n"
      "% a comment line\n"
      "0;0;0;8;4;4;16;-8\n"
      "0;4;4;4;4;1;27\n"
      "0;0;0;8;8;7;3\n"
      "0;0;0;8;8;7;5\n"
      "0;0;0;4;4;1;99999\n"  // out of range for short
      "0;0;0;4;4;9;1\n"      // undeclared type
      "0;0;0;4;4;4;1\n"      // vector without vec-y
      "nonsense\n"
      "0;-4;-4;1000;6;1;30\r\n";  // clipped to the grid: the top row of cells
  Bytes storage;
  const auto* fa = import_csv(text, 0, storage);
  REQUIRE(fa->grid() != nullptr);
  CHECK(to_vector(fa->grid()->mv0_x()) == std::vector<std::int16_t>{16, 16, 0, 0});
  CHECK(to_vector(fa->grid()->mv0_y()) == std::vector<std::int16_t>{-8, -8, 0, 0});
  CHECK(to_vector(fa->grid()->qp()) == std::vector<std::int16_t>{30, 30, -1, 27});
  REQUIRE(fa->diagnostics() != nullptr);
  std::vector<std::string> codes;
  for (const auto* d : *fa->diagnostics()) {
    codes.push_back(d->code()->str());
    if (d->code()->str() == "MDL-CSV-0001") {
      CHECK(d->severity() == fb::Severity::Warning);
      CHECK(d->message()->str().starts_with("4 malformed"));
      CHECK(d->message()->str().find("line 11") != std::string::npos);
    }
  }
  CHECK(codes == std::vector<std::string>{"MDL-CSV-0002", "MDL-CSV-0001"});

  for (const std::string& bad :
       {std::string{}, std::string{"0;0;0;4;4;1;30\n"}, std::string{"%;seq-specs;x;0;100000;8;25\n"},
        std::string{"%;seq-specs;x;0;0;8;25\n"}}) {
    Bytes b;
    const auto* none = import_csv(bad, 0, b);
    CHECK(none->grid() == nullptr);
    REQUIRE(none->diagnostics() != nullptr);
    CHECK(none->diagnostics()->Get(0)->code()->str() == "MDL-CSV-0004");
    CHECK(none->diagnostics()->Get(0)->severity() == fb::Severity::Error);
  }
}

TEST_CASE("yuview_csv: importer reads the hand-written sample files", "[model][yuview][fast]") {
  const std::filesystem::path dir = std::filesystem::path(OVT_SOURCE_DIR) / "core/tests/data/yuview";
  const auto codes_of = [](const fb::FrameAnalysis* fa) {
    std::vector<std::string> codes;
    if (fa->diagnostics() != nullptr) {
      for (const auto* d : *fa->diagnostics()) {
        codes.push_back(d->code()->str());
      }
    }
    return codes;
  };

  SECTION("foreign type names are reported, OVT types imported") {
    const std::string text = read_text(dir / "foreign_types.csv");
    Bytes b0;
    const auto* poc0 = import_csv(text, 0, b0);
    REQUIRE(poc0->grid() != nullptr);
    CHECK(poc0->grid()->cells_w() == 8);
    CHECK(poc0->grid()->cells_h() == 4);
    std::vector<std::int16_t> qp;
    for (int row = 0; row < 4; ++row) {
      qp.insert(qp.end(), 4, 30);
      qp.insert(qp.end(), 4, 31);
    }
    CHECK(to_vector(poc0->grid()->qp()) == qp);
    CHECK(codes_of(poc0) == std::vector<std::string>{"MDL-CSV-0002", "MDL-CSV-0002", "MDL-CSV-0002"});
    Bytes b1;
    const auto* poc1 = import_csv(text, 1, b1);
    CHECK(to_vector(poc1->grid()->qp()) == std::vector<std::int16_t>(32, 35));
    CHECK(codes_of(poc1) == std::vector<std::string>{"MDL-CSV-0002"});
  }

  SECTION("CRLF, unsorted rows, vector, map and range types") {
    Bytes b;
    const auto* fa = import_csv(read_text(dir / "ovt_mixed_poc.csv"), 2, b);
    REQUIRE(fa->grid() != nullptr);
    CHECK(to_vector(fa->grid()->mv0_x()) == std::vector<std::int16_t>{0, 0, -16, -16, 0, 0, -16, -16});
    CHECK(to_vector(fa->grid()->mv0_y()) == std::vector<std::int16_t>{-8, -8, 4, 4, -8, -8, 4, 4});
    CHECK(to_vector(fa->grid()->ref0()) == std::vector<std::int8_t>{0, 0, 1, 1, 0, 0, 1, 1});
    CHECK(to_vector(fa->grid()->partition_id()) == std::vector<std::uint16_t>{0, 0, 1, 1, 0, 0, 1, 1});
    CHECK(codes_of(fa).empty());
  }

  SECTION("malformed lines are counted, good rows still land") {
    Bytes b;
    const auto* fa = import_csv(read_text(dir / "malformed.csv"), 0, b);
    REQUIRE(fa->grid() != nullptr);
    CHECK(to_vector(fa->grid()->qp()) == std::vector<std::int16_t>(4, 27));
    REQUIRE(codes_of(fa) == std::vector<std::string>{"MDL-CSV-0001"});
    CHECK(fa->diagnostics()->Get(0)->message()->str() ==
          "7 malformed, out-of-range or undeclared line(s) skipped (first at line 4)");
  }
}
