// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.088: to_json() / from_json() of chapter 06 §6.8.1 on the WI-0.093 fixtures and on small StreamIndex,
// MetricsReport and ContainerRoot buffers built here with the object API (canonical by construction).
// The goldens are core/tests/golden/tiny16_hevc.json (acceptance test 1), tiny16_hevc.index.json and
// tiny_metrics.json (step 4); a mismatch writes `<name>.actual.json` into the test's binary directory.
// The cases tagged [json-files] write the exports that the CTest test_json_python_loads feeds to Python's
// json.loads (acceptance tests 3 and 4).

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ovt/cxx/model/canonical.hpp"
#include "ovt/cxx/model/json.hpp"
#include "ovt/cxx/model/schema_id.hpp"
#include "ovt_container_generated.h"
#include "ovt_metrics_generated.h"
#include "ovt_model_generated.h"

namespace {

using ovt::model::from_json;
using ovt::model::JsonOptions;
using ovt::model::SchemaId;
using ovt::model::to_json;
namespace fb = ovt::fb;
using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;

constexpr const char* kFrameAnalysis = "ovt.fb.FrameAnalysis";
constexpr const char* kStreamIndex = "ovt.fb.StreamIndex";
constexpr const char* kMetricsReport = "ovt.fb.MetricsReport";
constexpr const char* kContainerRoot = "ovt.fb.ContainerRoot";
constexpr const char* kHdrFrameMeasure = "ovt.fb.HdrFrameMeasure";
constexpr JsonOptions kCompact{.compact = true, .strict_numbers = false};
constexpr JsonOptions kStrict{.compact = false, .strict_numbers = true};

std::string read_golden(const std::string& name) {
  std::ifstream in(std::filesystem::path(OVT_GOLDEN_DIR) / name, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Bytes fixture(const std::string& name) {
  const std::string text = read_golden(name);
  return {text.begin(), text.end()};
}

void write_output(const std::string& name, const std::string& text) {
  std::ofstream(std::filesystem::path(OVT_TEST_OUTPUT_DIR) / name, std::ios::binary) << text;
}

void check_golden(const std::string& text, const std::string& stem) {
  const std::string golden = read_golden(stem + ".json");
  if (text != golden) {
    write_output(stem + ".actual.json", text);
  }
  CHECK(text == golden);
}

/// Pack @p native with the object API. @p identifier may be null (secondary root tables).
template <class Native>
Bytes pack(const Native& native, const char* identifier, bool force_defaults = false) {
  flatbuffers::FlatBufferBuilder builder;
  builder.ForceDefaults(force_defaults);
  builder.Finish(Native::TableType::Pack(builder, &native), identifier);
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

/// A 2-frame HEVC StreamIndex: the index half of the §6.10 stream.
Bytes tiny_index() {
  fb::StreamIndexT index;
  index.codec = fb::Codec::H265;
  index.schema_version = 1;
  index.source_hash.resize(32);
  for (std::size_t i = 0; i < index.source_hash.size(); ++i) {
    index.source_hash[i] = static_cast<std::uint8_t>(0x3b + 7 * i);
  }
  for (std::uint32_t i = 0; i < 2; ++i) {
    auto unit = std::make_unique<fb::UnitT>();
    unit->index = i;
    unit->file_span = std::make_unique<fb::ByteSpan>(4 + 147 * std::uint64_t{i}, 143);
    unit->prefix_span = std::make_unique<fb::ByteSpan>(147 * std::uint64_t{i}, 4);
    unit->type = i == 0 ? 19 : 1;
    unit->type_name = i == 0 ? "IDR_W_RADL" : "TRAIL_R";
    unit->frame = std::make_unique<fb::FrameId>(static_cast<std::int32_t>(i), static_cast<std::int32_t>(i));
    index.units.push_back(std::move(unit));
    auto frame = std::make_unique<fb::FrameInfoT>();
    frame->id = std::make_unique<fb::FrameId>(static_cast<std::int32_t>(i), static_cast<std::int32_t>(i));
    frame->type = i == 0 ? fb::FrameType::I : fb::FrameType::P;
    frame->shown = true;
    frame->is_rap = i == 0;
    frame->poc = i;
    frame->pts = 3600 * std::int64_t{i};
    frame->timebase_num = 1;
    frame->timebase_den = 90000;
    frame->size_bytes = 147;
    frame->units = {i};
    frame->coded_size = std::make_unique<fb::Rect>(0, 0, 16, 16);
    frame->qp_base = 32;
    if (i == 1) {
      frame->declared_refs = {0};
    }
    index.frames.push_back(std::move(frame));
  }
  index.display_order = {0, 1};
  auto diagnostic = std::make_unique<fb::DiagnosticT>();
  diagnostic->severity = fb::Severity::Warning;
  diagnostic->code = "SYN-H265-0007";
  diagnostic->message = "tab\there, quote \" and µ";
  index.diagnostics.push_back(std::move(diagnostic));
  return pack(index, nullptr);
}

/// A MetricsReport with one PSNR result of two frames; @p score is the first value of frame 1.
Bytes tiny_metrics(double score) {
  fb::MetricsReportT report;
  report.schema_version = 1;
  auto result = std::make_unique<fb::MetricsResultT>();
  result->descriptor = std::make_unique<fb::MetricDescriptorT>();
  result->descriptor->id = "psnr";
  result->descriptor->per_plane = true;
  result->descriptor->higher_is_better = true;
  result->descriptor->range_max = 100.0;
  for (std::int32_t i = 0; i < 2; ++i) {
    auto frame = std::make_unique<fb::FrameScoreT>();
    frame->frame = std::make_unique<fb::FrameId>(i, i);
    frame->values = {i == 0 ? 41.25 : score, 0.1, 1e-7};
    result->frames.push_back(std::move(frame));
  }
  result->pooled_mean = {0.1 + 0.2, 1e21, -0.0};
  result->alignment_confidence = 0.5;
  result->engine = "ovt-kernels 1.0";
  result->schema_version = 1;
  result->frames_total = 2;
  result->frames_scored = 2;
  result->distorted_source_hash = {0x00, 0x0f, 0xa0, 0xff};
  report.results.push_back(std::move(result));
  report.reference_name = "ref.y4m";
  report.distorted_name = "dist.mp4";
  return pack(report, fb::MetricsReportIdentifier());
}

/// A ContainerRoot with every kind of hash-like field: source_hash, a uuid box, Field.bytes, pssh ids.
Bytes tiny_container() {
  fb::ContainerRootT root;
  root.schema_version = 1;
  root.file_size = (std::uint64_t{1} << 53U) + 1;  // beyond 2^53: a decimal string
  root.source_hash = {0xde, 0xad, 0xbe, 0xef};
  root.tree = std::make_unique<fb::ContainerTreeT>();
  auto box = std::make_unique<fb::BoxNodeT>();
  box->fourcc = "uuid";
  box->uuid = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  box->file_span = std::make_unique<fb::ByteSpan>(std::uint64_t{1} << 53U, 24);  // exactly 2^53: a number
  auto field = std::make_unique<fb::FieldT>();
  field->name = "payload";
  field->kind = fb::SyntaxValueKind::Bytes;
  field->bytes = {0x00, 0x7f, 0x80};
  box->fields.push_back(std::move(field));
  root.tree->root.push_back(std::move(box));
  return pack(root, fb::ContainerRootIdentifier());
}

/// The fixture with @p edit applied (UnPack, edit, Pack with the `OVTA` identifier).
template <class Edit>
Bytes edited_fixture(Edit&& edit, bool force_defaults = false) {
  const Bytes bytes = fixture("tiny16_hevc.fa");
  fb::FrameAnalysisT native;
  fb::GetFrameAnalysis(bytes.data())->UnPackTo(&native);
  edit(native);
  return pack(native, fb::FrameAnalysisIdentifier(), force_defaults);
}

/// The fixture with a FrameType the schema does not name.
Bytes odd_enum() {
  return edited_fixture([](fb::FrameAnalysisT& frame) { frame.frame->type = static_cast<fb::FrameType>(200); });
}

/// A FrameInfo whose `shown` byte is 2 (only reachable without the generated API); not canonical.
Bytes shown_two() {
  flatbuffers::FlatBufferBuilder raw;
  const auto start = raw.StartTable();
  raw.AddElement<std::uint8_t>(fb::FrameInfo::VT_SHOWN, 2, 0);
  raw.AddElement<std::uint8_t>(fb::FrameInfo::VT_IS_RAP, 1, 0);
  raw.Finish(flatbuffers::Offset<flatbuffers::Table>(raw.EndTable(start)));
  return {raw.GetBufferPointer(), raw.GetBufferPointer() + raw.GetSize()};
}

Bytes float_measure() {
  fb::HdrFrameMeasureT native;
  native.max_nits = 203.0F;
  native.fall_nits = 0.1F;
  native.p99_nits = 1e-10F;
  native.min_nits = 16777217.0F;  // not a float: stored as 16777216
  return pack(native, nullptr);
}

/// Every escape class: quote, backslash, the short escapes, other controls, DEL, 2/3/4-byte UTF-8, NUL.
const std::string kOddString =
    std::string("a\"b\\c\n\r\t\b\f\x01\x1f\x7f é \xe2\x82\xac \xf0\x9f\x8e\xac") + '\0' + "z";

Bytes odd_string() {
  return edited_fixture([](fb::FrameAnalysisT& frame) { frame.plugin_id = kOddString; });
}

/// The fixture with a block whose `ext_type` names no table.
Bytes untyped_ext() {
  return edited_fixture([](fb::FrameAnalysisT& frame) { frame.blocks[2]->ext_type = "Nope"; });
}

/// An HevcCu whose `palette` holds @p palette (a finished HevcPalette buffer, or any bytes).
Bytes palette_cu(const Bytes& palette) {
  fb::HevcCuT cu;
  cu.palette_mode_flag = 1;
  cu.palette = palette;
  return pack(cu, nullptr);
}

Bytes hevc_palette() {
  fb::HevcPaletteT palette;
  palette.num_signalled_palette_entries = 2;
  palette.new_palette_entries = {512, 1023};
  palette.PaletteIndexMap = {0, 1, 1, 0};
  return pack(palette, nullptr);
}

/// Strict RFC 8259 parse (nlohmann rejects bare nan / inf tokens).
Json parse_strict(const std::string& text) {
  return Json::parse(text, nullptr, true, false);
}

}  // namespace

TEST_CASE("to_json exports tiny16_hevc.fa as the golden JSON (WI-0.088 AT1)", "[model][json][golden]") {
  const Bytes bytes = fixture("tiny16_hevc.fa");
  const std::string text = to_json(bytes, SchemaId::Model, kFrameAnalysis);
  check_golden(text, "tiny16_hevc");
  CHECK(to_json(bytes, SchemaId::Model, kFrameAnalysis) == text);  // deterministic (§0.7.5)

  // The §6.8.1 rules on that text: schema order, enums by name, every scalar, nested buffers in place.
  CHECK(text.starts_with("{\n  \"schema_version\": 1,\n  \"plugin_id\": \"ovt-plugin-de265\",\n"));
  CHECK(text.ends_with("}\n"));
  const Json json = parse_strict(text);
  CHECK(json["frame"]["type"] == "I");
  CHECK(json["frame"]["show_existing_of"] == -1);
  CHECK(json["frame"]["ref_state_type"] == "HevcRps");
  CHECK(json["frame"]["ref_state"].is_object());
  CHECK(json["grid"]["cells_w"] == 4);
  CHECK(json["grid"]["block_id"].size() == 16);
  const auto& blocks = json["blocks"];
  REQUIRE(blocks.size() > 2);
  CHECK(blocks[0]["kind"] == "Other");
  CHECK(blocks[0]["rect"] == Json({{"x", 0}, {"y", 0}, {"w", 16}, {"h", 16}}));
  CHECK_FALSE(blocks[0].contains("ext"));  // absent: left out
  CHECK(blocks[1]["ext_type"] == "HevcCtu");
  CHECK(blocks[1]["ext"].is_object());
  CHECK(blocks[2]["ext_type"] == "HevcCu");
  CHECK(blocks[2]["ext"].contains("cu_skip_flag"));
}

TEST_CASE("compact export is one line of strict JSON (WI-0.088 AT3)", "[model][json][json-files]") {
  const Bytes bytes = fixture("tiny16_hevc.fa");
  const std::string text = to_json(bytes, SchemaId::Model, kFrameAnalysis, kCompact);
  CHECK(text.find('\n') == std::string::npos);
  CHECK(text.starts_with("{\"schema_version\": 1,\"plugin_id\": \"ovt-plugin-de265\","));
  CHECK(parse_strict(text) == parse_strict(to_json(bytes, SchemaId::Model, kFrameAnalysis)));
  write_output("tiny16_hevc.compact.json", text);
  for (const char* name : {"tiny32_h264.fa", "tiny64_av1.fa"}) {  // the other fixtures: same content either way
    const Bytes other = fixture(name);
    const std::string one_line = to_json(other, SchemaId::Model, kFrameAnalysis, kCompact);
    CHECK(one_line.find('\n') == std::string::npos);
    CHECK(parse_strict(one_line) == parse_strict(to_json(other, SchemaId::Model, kFrameAnalysis, kStrict)));
  }
}

TEST_CASE("a NaN score is the string \"nan\" with strict_numbers (WI-0.088 AT4)", "[model][json][json-files]") {
  const Bytes bytes = tiny_metrics(std::numeric_limits<double>::quiet_NaN());
  const std::string strict = to_json(bytes, SchemaId::Metrics, kMetricsReport, kStrict);
  const Json json = parse_strict(strict);
  CHECK(json["results"][0]["frames"][1]["values"][0] == "nan");
  write_output("tiny_metrics_nan.strict.json", strict);

  // Without the option the token is bare, which strict JSON parsers reject.
  const std::string loose = to_json(bytes, SchemaId::Metrics, kMetricsReport);
  CHECK(loose.find("\n            nan,\n") != std::string::npos);
  CHECK_THROWS_AS(parse_strict(loose), Json::parse_error);

  for (const auto& [value, token] : {std::pair{std::numeric_limits<double>::infinity(), "inf"},
                                     std::pair{-std::numeric_limits<double>::infinity(), "-inf"}}) {
    const std::string text = to_json(tiny_metrics(value), SchemaId::Metrics, kMetricsReport, kStrict);
    CHECK(parse_strict(text)["results"][0]["frames"][1]["values"][0] == token);
  }
}

TEST_CASE("an index and a metrics report match their goldens (WI-0.088 step 4)", "[model][json][golden]") {
  const Bytes index = tiny_index();
  const std::string index_text = to_json(index, SchemaId::Model, kStreamIndex);
  check_golden(index_text, "tiny16_hevc.index");
  const Json index_json = parse_strict(index_text);
  CHECK(index_json["codec"] == "H265");
  CHECK(index_json["source_hash"] == "3b424950575e656c737a81888f969da4abb2b9c0c7ced5dce3eaf1f8ff060d14");
  CHECK(index_json["units"][1]["file_span"] == Json({{"offset", 151}, {"length", 143}}));
  CHECK(index_json["diagnostics"][0]["message"] == "tab\there, quote \" and µ");
  CHECK(index_text.find(R"("message": "tab\there, quote \" and µ")") != std::string::npos);  // natural UTF-8

  const Bytes metrics = tiny_metrics(38.5);
  const std::string metrics_text = to_json(metrics, SchemaId::Metrics, kMetricsReport);
  check_golden(metrics_text, "tiny_metrics");
  // Shortest round-trip doubles (WI-0.081 pitfall): no digits lost, none invented.
  const Json values = parse_strict(metrics_text)["results"][0];
  CHECK(metrics_text.find("\n            41.25,\n            0.1,\n            1e-7\n") != std::string::npos);
  CHECK(metrics_text.find("\n        0.30000000000000004,\n        1e+21,\n        -0\n") != std::string::npos);
  CHECK(values["distorted_source_hash"] == "000fa0ff");
  CHECK(values["descriptor"]["kind"] == "FullReference");
}

TEST_CASE("hash-like byte vectors are hex and 64-bit integers beyond 2^53 are strings", "[model][json]") {
  const Bytes bytes = tiny_container();
  const Json json = parse_strict(to_json(bytes, SchemaId::Container, kContainerRoot));
  CHECK(json["source_hash"] == "deadbeef");
  CHECK(json["file_size"] == "9007199254740993");
  const auto& box = json["tree"]["root"][0];
  CHECK(box["uuid"] == "0123456789abcdef0123456789abcdef");
  CHECK(box["file_span"]["offset"] == 9007199254740992);
  CHECK(box["fields"][0]["bytes"] == "007f80");
  CHECK(box["version"] == -1);    // a scalar at its default is written
  fb::ExtractManifestT manifest;  // ExtractManifest.bytes is a ulong, not a hash
  manifest.bytes = 12;
  CHECK(parse_strict(to_json(pack(manifest, nullptr), SchemaId::Model, "ovt.fb.ExtractManifest"))["bytes"] == 12);
}

TEST_CASE("to_json writes values, not layout", "[model][json]") {
  // A buffer packed with ForceDefaults has another layout and the same values: the same text.
  const Bytes forced = edited_fixture([](fb::FrameAnalysisT&) {}, true);
  REQUIRE(forced != fixture("tiny16_hevc.fa"));
  CHECK(to_json(forced, SchemaId::Model, kFrameAnalysis) == read_golden("tiny16_hevc.json"));

  // An unknown enum value is its integer, and so is a bool byte above 1; a present empty vector is [].
  const std::string text = to_json(odd_enum(), SchemaId::Model, kFrameAnalysis);
  CHECK(parse_strict(text)["frame"]["type"] == 200);
  CHECK(parse_strict(text)["frame"]["is_rap"] == true);
  const std::string two_text = to_json(shown_two(), SchemaId::Model, "ovt.fb.FrameInfo", kCompact);
  CHECK(two_text.find("\"shown\": 2,") != std::string::npos);
  CHECK(two_text.find("\"is_rap\": true,") != std::string::npos);
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(fb::CreateFrameScore(builder, nullptr, builder.CreateVector(std::vector<double>{})));
  const Bytes empty(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
  CHECK(to_json(empty, SchemaId::Metrics, "ovt.fb.FrameScore", kCompact) == "{\"values\": [],\"map_block_size\": 0}");

  // Floats print as the shortest text of the float, not of its double.
  const std::string float_text = to_json(float_measure(), SchemaId::Model, kHdrFrameMeasure, kCompact);
  CHECK(float_text.find("\"max_nits\": 203,\"fall_nits\": 0.1,\"p99_nits\": 1e-10,\"min_nits\": 16777216,") !=
        std::string::npos);

  // One layout whatever the fmt version: plain digits from 1e-6 up to 21 integer digits, an exponent beyond.
  const auto score = [](double value) {
    return to_json(tiny_metrics(value), SchemaId::Metrics, kMetricsReport, kCompact);
  };
  for (const auto& [value, want] :
       std::vector<std::pair<double, const char*>>{{1e20, "100000000000000000000"},
                                                   {1.5e21, "1.5e+21"},
                                                   {1e21, "1e+21"},
                                                   {0.000001, "0.000001"},
                                                   {1.5e-7, "1.5e-7"},
                                                   {-2.5e-10, "-2.5e-10"},
                                                   {123.456, "123.456"},
                                                   {-0.0, "-0"},
                                                   {5e-324, "5e-324"},
                                                   {1.7976931348623157e308, "1.7976931348623157e+308"}}) {
    CAPTURE(want);
    CHECK(score(value).find(std::string("[") + want + ",0.1,1e-7]") != std::string::npos);
  }
}

TEST_CASE("strings are escaped as JSON and must be UTF-8", "[model][json]") {
  const std::string text = to_json(odd_string(), SchemaId::Model, kFrameAnalysis, kCompact);
  CHECK(
      text.find(
          "\"plugin_id\": \"a\\\"b\\\\c\\n\\r\\t\\b\\f\\u0001\\u001f\x7f é \xe2\x82\xac \xf0\x9f\x8e\xac\\u0000z\"") !=
      std::string::npos);
  CHECK(parse_strict(text)["plugin_id"] == kOddString);

  for (const char* bad : {"\xff", "\xc3", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"}) {
    const Bytes invalid = edited_fixture([&](fb::FrameAnalysisT& frame) { frame.plugin_id = bad; });
    CHECK_THROWS_AS(to_json(invalid, SchemaId::Model, kFrameAnalysis), std::invalid_argument);
  }
}

TEST_CASE("nested buffers follow their _type sibling and bad input is refused", "[model][json]") {
  // `ext_type` names no table: the bytes stay an array of numbers.
  const std::string text = to_json(untyped_ext(), SchemaId::Model, kFrameAnalysis);
  CHECK(parse_strict(text)["blocks"][2]["ext"].is_array());

  // `ext_type` names a table the bytes are not: no JSON.
  const Bytes broken = edited_fixture([](fb::FrameAnalysisT& frame) { frame.blocks[2]->ext = {1, 2, 3}; });
  CHECK_THROWS_AS(to_json(broken, SchemaId::Model, kFrameAnalysis), std::invalid_argument);

  // The two palette fields are nested buffers of a fixed table, without a `_type` sibling (§6.8.1).
  const Json cu = parse_strict(to_json(palette_cu(hevc_palette()), SchemaId::Model, "ovt.fb.HevcCu"));
  REQUIRE(cu["palette"].is_object());
  CHECK(cu["palette"]["num_signalled_palette_entries"] == 2);
  CHECK(cu["palette"]["new_palette_entries"] == Json::array({512, 1023}));
  CHECK(cu["palette"]["PaletteIndexMap"] == Json::array({0, 1, 1, 0}));
  CHECK(to_json(palette_cu(hevc_palette()), SchemaId::Model, "ovt.fb.HevcCu", kCompact).find("\"palette\": {") !=
        std::string::npos);

  fb::Av1PaletteT colors;
  colors.palette_colors_y = {16, 235};
  colors.ColorMapY = {1, 0};
  fb::Av1BlockT av1;
  av1.PaletteSizeY = 2;
  av1.palette = pack(colors, nullptr);
  const Json av1_json = parse_strict(to_json(pack(av1, nullptr), SchemaId::Model, "ovt.fb.Av1Block"));
  CHECK(av1_json["palette"]["palette_colors_y"] == Json::array({16, 235}));
  CHECK(av1_json["palette"]["ColorMapY"] == Json::array({1, 0}));

  // Inside a block's ext the palette is the second nested level.
  const Bytes in_frame = edited_fixture([](fb::FrameAnalysisT& frame) {
    REQUIRE(frame.blocks[2]->ext_type == "HevcCu");
    frame.blocks[2]->ext = palette_cu(hevc_palette());
  });
  const Json framed = parse_strict(to_json(in_frame, SchemaId::Model, kFrameAnalysis));
  CHECK(framed["blocks"][2]["ext"]["palette"]["new_palette_entries"] == Json::array({512, 1023}));

  // Bytes that are no palette table give no JSON; a CU without a palette has no such key.
  CHECK_THROWS_AS(to_json(palette_cu({1, 2, 3}), SchemaId::Model, "ovt.fb.HevcCu"), std::invalid_argument);
  CHECK_FALSE(
      parse_strict(to_json(fixture("tiny16_hevc.fa"), SchemaId::Model, kFrameAnalysis))["blocks"][2]["ext"].contains(
          "palette"));

  const Bytes bytes = fixture("tiny16_hevc.fa");
  CHECK_THROWS_AS(to_json(bytes, SchemaId::Model, "ovt.fb.NoSuchTable"), std::invalid_argument);
  CHECK_THROWS_AS(to_json(bytes, SchemaId::Model, "ovt.fb.Rect"), std::invalid_argument);  // a struct is no root table
  CHECK_THROWS_AS(to_json(bytes, static_cast<SchemaId>(9), kFrameAnalysis), std::invalid_argument);
  CHECK_THROWS_AS(to_json(Bytes(bytes.begin(), bytes.begin() + 40), SchemaId::Model, kFrameAnalysis),
                  std::invalid_argument);
  CHECK_THROWS_AS(to_json({}, SchemaId::Model, kFrameAnalysis), std::invalid_argument);
}

// ---- from_json (part b) ---------------------------------------------------------------------------------

TEST_CASE("from_json restores the fixture bytes from the golden JSON (WI-0.088 AT2)", "[model][json][golden]") {
  const Bytes bytes = fixture("tiny16_hevc.fa");
  const Bytes imported = from_json(read_golden("tiny16_hevc.json"), SchemaId::Model, kFrameAnalysis);
  CHECK(ovt::model::canonicalize(imported, SchemaId::Model, kFrameAnalysis) == bytes);
  CHECK(imported == bytes);  // from_json already returns canonical bytes
}

TEST_CASE("every fixture round-trips through each JSON layout", "[model][json][golden]") {
  for (const char* name : {"tiny16_hevc.fa", "tiny32_h264.fa", "tiny64_av1.fa"}) {
    const Bytes bytes = fixture(name);
    for (const JsonOptions options : {JsonOptions{}, kCompact, kStrict}) {
      const std::string text = to_json(bytes, SchemaId::Model, kFrameAnalysis, options);
      INFO(name << " compact=" << options.compact << " strict=" << options.strict_numbers);
      CHECK(from_json(text, SchemaId::Model, kFrameAnalysis) == bytes);
    }
  }
}

TEST_CASE("every value rule of the exporter round-trips", "[model][json]") {
  /// from_json(to_json(bytes)) in both number styles is the canonical form of @p bytes.
  const auto round_trips = [](const Bytes& bytes, SchemaId schema, const char* root) {
    INFO(root);
    const Bytes canonical = ovt::model::canonicalize(bytes, schema, root);
    CHECK(from_json(to_json(bytes, schema, root), schema, root) == canonical);
    CHECK(from_json(to_json(bytes, schema, root, {.compact = true, .strict_numbers = true}), schema, root) ==
          canonical);
  };
  // NaN and the infinities, bare and quoted; shortest doubles; a hash; -0.
  for (const double score : {38.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()}) {
    round_trips(tiny_metrics(score), SchemaId::Metrics, kMetricsReport);
  }
  // A secondary root table gets no identifier (§6.1.1): the bytes of the index come back as they were.
  const Bytes index = tiny_index();
  round_trips(index, SchemaId::Model, kStreamIndex);
  CHECK(from_json(read_golden("tiny16_hevc.index.json"), SchemaId::Model, kStreamIndex) == index);
  CHECK(from_json(read_golden("tiny_metrics.json"), SchemaId::Metrics, kMetricsReport) == tiny_metrics(38.5));
  round_trips(tiny_container(), SchemaId::Container, kContainerRoot);  // hex fields, integers beyond 2^53
  round_trips(odd_enum(), SchemaId::Model, kFrameAnalysis);            // an enum value as its integer
  round_trips(shown_two(), SchemaId::Model, "ovt.fb.FrameInfo");       // a bool byte above 1
  round_trips(float_measure(), SchemaId::Model, kHdrFrameMeasure);     // floats
  round_trips(odd_string(), SchemaId::Model, kFrameAnalysis);          // every string escape
  round_trips(untyped_ext(), SchemaId::Model, kFrameAnalysis);         // a nested field left as bytes

  // The importer also takes the FlatBuffers form of a hash (an array of numbers), upper-case hex and quoted
  // numbers; a number under ExtractManifest.bytes is that ulong, not a hash.
  const std::string text = R"({"schema_version": "1", "file_size": 9007199254740993,
    "source_hash": [222, 173, 190, 239],
    "tree": {"root": [{"fourcc": "uuid", "uuid": "0123456789ABCDEF0123456789abcdef",
      "file_span": {"offset": 9007199254740992, "length": 24},
      "fields": [{"name": "payload", "kind": "Bytes", "bytes": "007F80"}]}]}})";
  CHECK(from_json(text, SchemaId::Container, kContainerRoot) == tiny_container());
  const Bytes manifest = from_json(R"({"path": "a.bin", "bytes": 12})", SchemaId::Model, "ovt.fb.ExtractManifest");
  CHECK(flatbuffers::GetRoot<fb::ExtractManifest>(manifest.data())->bytes() == 12);
}

TEST_CASE("from_json finds the _type sibling of a nested object", "[model][json]") {
  // `X_type` may come before or after `X` (FrameInfo declares ref_state first, Block ext_type first).
  const Bytes bytes = fixture("tiny16_hevc.fa");
  Json json = parse_strict(read_golden("tiny16_hevc.json"));
  CHECK(from_json(json.dump(), SchemaId::Model, kFrameAnalysis) == bytes);  // nlohmann sorts the keys by name
  json["blocks"][2].erase("ext_type");
  CHECK_THROWS_AS(from_json(json.dump(), SchemaId::Model, kFrameAnalysis), std::invalid_argument);
  json["blocks"][2]["ext_type"] = "Nope";
  CHECK_THROWS_AS(from_json(json.dump(), SchemaId::Model, kFrameAnalysis), std::invalid_argument);
  json["blocks"][2]["ext_type"] = "FrameAnalysis";  // an object of the wrong table: unknown keys are ignored
  CHECK_NOTHROW(from_json(json.dump(), SchemaId::Model, kFrameAnalysis));
}

TEST_CASE("from_json rebuilds the fixed-type palette nested buffers", "[model][json]") {
  using ovt::model::canonicalize;
  using ovt::model::structural_diff;
  // A palette packed with ForceDefaults is not canonical: canonicalize reaches into it (the nested table is
  // fixed by the schema, §6.8.1), and the import of its JSON gives the same bytes.
  fb::HevcPaletteT native;
  native.num_signalled_palette_entries = 2;
  native.new_palette_entries = {512, 1023};
  native.PaletteIndexMap = {0, 1, 1, 0};
  const Bytes forced = palette_cu(pack(native, nullptr, true));
  const Bytes plain = palette_cu(hevc_palette());
  const Bytes canonical = canonicalize(forced, SchemaId::Model, "ovt.fb.HevcCu");
  CHECK(canonical == canonicalize(plain, SchemaId::Model, "ovt.fb.HevcCu"));
  CHECK(canonical != canonicalize(palette_cu({}), SchemaId::Model, "ovt.fb.HevcCu"));
  CHECK(structural_diff(forced, plain, SchemaId::Model, "ovt.fb.HevcCu").empty());
  for (const JsonOptions options : {JsonOptions{}, kCompact}) {
    const std::string text = to_json(forced, SchemaId::Model, "ovt.fb.HevcCu", options);
    CHECK(from_json(text, SchemaId::Model, "ovt.fb.HevcCu") == canonical);
  }

  fb::Av1PaletteT colors;
  colors.palette_colors_y = {16, 235};
  colors.ColorMapY = {1, 0};
  fb::Av1BlockT av1;
  av1.PaletteSizeY = 2;
  av1.palette = pack(colors, nullptr, true);
  const Bytes block = pack(av1, nullptr);
  CHECK(from_json(to_json(block, SchemaId::Model, "ovt.fb.Av1Block"), SchemaId::Model, "ovt.fb.Av1Block") ==
        canonicalize(block, SchemaId::Model, "ovt.fb.Av1Block"));

  // Two nested levels: the palette of the CU in a block's ext.
  const Bytes in_frame = edited_fixture([&](fb::FrameAnalysisT& frame) { frame.blocks[2]->ext = forced; });
  const Bytes imported = from_json(to_json(in_frame, SchemaId::Model, kFrameAnalysis), SchemaId::Model, kFrameAnalysis);
  CHECK(imported == canonicalize(in_frame, SchemaId::Model, kFrameAnalysis));
  CHECK(structural_diff(in_frame, imported, SchemaId::Model, kFrameAnalysis).empty());

  // An array of numbers is still accepted for a palette (the bytes as they are), when they are a palette.
  Json json = parse_strict(to_json(plain, SchemaId::Model, "ovt.fb.HevcCu"));
  json["palette"] = hevc_palette();
  CHECK(from_json(json.dump(), SchemaId::Model, "ovt.fb.HevcCu") == canonical);
  json["palette"] = Json::array({1, 2, 3});
  CHECK_THROWS_AS(from_json(json.dump(), SchemaId::Model, "ovt.fb.HevcCu"), std::invalid_argument);
}

TEST_CASE("from_json ignores unknown keys and rejects malformed text", "[model][json]") {
  const Bytes bytes = fixture("tiny16_hevc.fa");
  Json json = parse_strict(read_golden("tiny16_hevc.json"));
  json["added_in_1_1"] = Json({{"deep", {1, 2, {{"x", nullptr}}}}});
  json["blocks"][0]["future"] = "value";
  CHECK(from_json(json.dump(2), SchemaId::Model, kFrameAnalysis) == bytes);
  json["frame"].erase("shown");  // a missing key is the default (false)
  const Bytes hidden = from_json(json.dump(), SchemaId::Model, kFrameAnalysis);
  CHECK_FALSE(fb::GetFrameAnalysis(hidden.data())->frame()->shown());

  const auto rejects = [](std::string_view text, const char* root = kFrameAnalysis, SchemaId id = SchemaId::Model) {
    INFO(text);
    CHECK_THROWS_AS(from_json(text, id, root), std::invalid_argument);
  };
  rejects("");
  rejects("[1]");
  rejects(R"({"schema_version": 1} x)");
  rejects(R"({"schema_version": 1)");
  rejects(R"({"schema_version" 1})");
  rejects(R"({"schema_version": 1 "plugin_id": "a"})");
  rejects("{schema_version: 1}");
  rejects(R"({"schema_version": })");
  rejects(R"({"schema_version": 4294967296})");
  rejects(R"({"schema_version": 1, "schema_version": 1})");
  rejects(R"({"plugin_id": "abc)");
  rejects(R"({"plugin_id": "abc\)");
  rejects(R"({"frame": {"type": "NoSuchType"}})");
  rejects(R"({"blocks": [{"ext_type": "HevcCu", "ext": {"cu_skip_flag": 1}}, ]})");
  rejects(std::string_view("{\"schema_version\": 1}\0", 22));
  rejects(R"({"source_hash": "abc"})", kStreamIndex);
  rejects(R"({"source_hash": "zz"})", kStreamIndex);
  rejects("{}", "ovt.fb.NoSuchTable");
  rejects("{}", "ovt.fb.Rect");  // a struct is no root table
  rejects("{}", kFrameAnalysis, static_cast<SchemaId>(9));

  // Tables nested past the limit (SyntaxNode.children), and nested buffers past theirs (4 levels).
  const auto nodes = [](int depth) {
    std::string text;
    for (int i = 0; i < depth; ++i) {
      text += R"({"name": "n", "children": [)";
    }
    for (int i = 0; i < depth; ++i) {
      text += "]}";
    }
    return text;
  };
  CHECK_NOTHROW(from_json(nodes(8), SchemaId::Model, "ovt.fb.SyntaxNode"));
  rejects(nodes(70), "ovt.fb.SyntaxNode");
  const auto buffers = [](int levels) {
    std::string text = "{}";
    for (int i = 0; i < levels; ++i) {
      text.insert(0, R"({"filter_params_type": "FrameAnalysis", "filter_params": )");
      text += '}';
    }
    return text;
  };
  const Bytes four = from_json(buffers(4), SchemaId::Model, kFrameAnalysis);
  CHECK(from_json(to_json(four, SchemaId::Model, kFrameAnalysis), SchemaId::Model, kFrameAnalysis) == four);
  rejects(buffers(5));
  CHECK_NOTHROW(from_json("{}", SchemaId::Model, kFrameAnalysis));
}
