// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of UnitListModel (WI-1.459; chapter 11 §11.3.6). Acceptance tests 1 and 2 of the WI name the fixture
// `syn_sei_h264_ga94_cc`, whose StreamIndex the GUI tests cannot load yet. SPEC-ASSUMPTION(#240): the tests build an
// equivalent H.264 index in memory (AUD, SPS, PPS, SEI user data, IDR and non-IDR slices, a second SEI) instead.

#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "models/selection_model.h"
#include "models/stream_index_adapter.h"
#include "models/stream_index_model.h"
#include "models/unit_list_model.h"
#include "ovt_model_generated.h"
#include "qt_test_app.h"

namespace {

using namespace ovt::app;  // NOLINT(google-build-using-namespace): test file

struct UnitSpec {
  const char* name;
  int type;
  int decode;   // frame decode index, -1 for non-VCL
  int display;  // frame display index
  int diagnostics = 0;
  int layer = 0;
  int tid = 0;
};

// Units in decode order, laid out as Annex B with 4-byte start codes; the sizes grow with the unit index so that
// sorting by size differs from sorting by number. Three frames: decode order I P B has display order I B P.
std::vector<std::uint8_t> build_index(const std::vector<UnitSpec>& specs) {
  ovt::fb::StreamIndexT index;
  index.codec = ovt::fb::Codec::H264;
  index.schema_version = 1;
  index.display_order = {0, 2, 1};
  for (int f = 0; f < 3; ++f) {
    auto fi = std::make_unique<ovt::fb::FrameInfoT>();
    const int display = f == 0 ? 0 : (f == 1 ? 2 : 1);
    fi->id = std::make_unique<ovt::fb::FrameId>(f, display);
    fi->is_rap = f == 0;
    fi->shown = true;
    index.frames.push_back(std::move(fi));
  }
  std::uint64_t offset = 0;
  for (std::size_t i = 0; i < specs.size(); ++i) {
    const UnitSpec& s = specs[i];
    auto u = std::make_unique<ovt::fb::UnitT>();
    u->index = static_cast<std::uint32_t>(i);
    const std::uint64_t payload = 10 + 5 * (specs.size() - i);
    u->prefix_span = std::make_unique<ovt::fb::ByteSpan>(offset, 4);
    u->file_span = std::make_unique<ovt::fb::ByteSpan>(offset + 4, payload);
    offset += 4 + payload;
    u->type = static_cast<std::uint8_t>(s.type);
    u->type_name = s.name;
    u->layer_id = static_cast<std::uint8_t>(s.layer);
    u->temporal_id = static_cast<std::uint8_t>(s.tid);
    u->frame = std::make_unique<ovt::fb::FrameId>(s.decode, s.display);
    for (int d = 0; d < s.diagnostics; ++d) {
      auto diag = std::make_unique<ovt::fb::DiagnosticT>();
      diag->code = "SYN-TEST-0001";
      u->diagnostics.push_back(std::move(diag));
    }
    if (s.decode >= 0) {
      index.frames[static_cast<std::size_t>(s.decode)]->units.push_back(u->index);
    }
    index.units.push_back(std::move(u));
  }
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(ovt::fb::StreamIndex::Pack(fbb, &index));
  const auto bytes = fbb.GetBufferSpan();
  return {bytes.begin(), bytes.end()};
}

// AUD SPS PPS SEI IDR | AUD SEI SLICE(P, decode 1) | AUD SLICE(B, decode 2, 2 diagnostics)
std::vector<UnitSpec> sei_stream() {
  return {{"AUD", 9, -1, -1},     {"SPS", 7, -1, -1},         {"PPS", 8, -1, -1},    {"SEI", 6, -1, -1},
          {"SLICE_IDR", 5, 0, 0}, {"AUD", 9, -1, -1},         {"SEI", 6, -1, -1, 1}, {"SLICE", 1, 1, 2, 0, 0, 1},
          {"AUD", 9, -1, -1},     {"SLICE", 1, 2, 1, 2, 0, 1}};
}

std::vector<int> units_of(const UnitListModel& model) {
  std::vector<int> out;
  for (int r = 0; r < model.rowCount(); ++r) {
    out.push_back(model.unitAt(r));
  }
  return out;
}

QString cell(const UnitListModel& model, int row, int column) {
  return model.data(model.index(row, column)).toString();
}

}  // namespace

TEST_CASE("UnitListModel lists every unit in decode order with the §11.3.6 columns", "[app][unitlist]") {
  ovt::app::testing::qt_app();
  UnitListModel model;
  model.setHandle(StreamIndexHandle::adopt(build_index(sei_stream())));
  REQUIRE(model.rowCount() == 10);
  CHECK(model.columnCount() == 8);
  CHECK(model.unit_count() == 10);
  CHECK(model.headerData(UnitListModel::ColType, Qt::Horizontal).toString() == QStringLiteral("Type"));

  CHECK(cell(model, 4, UnitListModel::ColNumber) == QStringLiteral("4"));
  CHECK(cell(model, 4, UnitListModel::ColType) == QStringLiteral("SLICE_IDR"));
  CHECK(cell(model, 7, UnitListModel::ColLayerTemporal) == QStringLiteral("0/1"));
  CHECK(cell(model, 0, UnitListModel::ColOffset) == QStringLiteral("4"));
  CHECK(cell(model, 0, UnitListModel::ColSize) == QStringLiteral("60"));
  CHECK(cell(model, 7, UnitListModel::ColFrame) == QStringLiteral("1/2"));  // decode/display
  CHECK(cell(model, 3, UnitListModel::ColFrame).isEmpty());                 // non-VCL: blank
  CHECK(cell(model, 4, UnitListModel::ColFlags) == QStringLiteral("RAP"));
  CHECK(cell(model, 9, UnitListModel::ColFlags) == QString::fromUtf8("\xE2\x9A\xA0") + QStringLiteral("2"));
  CHECK(cell(model, 7, UnitListModel::ColFlags).isEmpty());

  model.set_hex_numbers(true);
  CHECK(cell(model, 0, UnitListModel::ColOffset) == QStringLiteral("0x00000004"));
  CHECK(cell(model, 0, UnitListModel::ColSize) == QStringLiteral("0x0000003C"));
}

TEST_CASE("unit classes tint VCL, parameter sets, SEI and AUD", "[app][unitlist]") {
  ovt::app::testing::qt_app();
  UnitListModel model;
  model.setHandle(StreamIndexHandle::adopt(build_index(sei_stream())));
  const auto cls = [&](int row) { return model.data(model.index(row, 0), UnitListModel::ClassRole).toString(); };
  CHECK(cls(0) == QStringLiteral("aud"));
  CHECK(cls(1) == QStringLiteral("parameterSet"));
  CHECK(cls(3) == QStringLiteral("sei"));
  CHECK(cls(4) == QStringLiteral("vcl"));

  using ovt::fb::Codec;
  CHECK(classify_unit(Codec::H264, 12, false) == UnitClass::Aud);
  CHECK(classify_unit(Codec::H264, 1, true) == UnitClass::Vcl);
  CHECK(classify_unit(Codec::H265, 33, false) == UnitClass::ParameterSet);
  CHECK(classify_unit(Codec::H265, 39, false) == UnitClass::Sei);
  CHECK(classify_unit(Codec::H265, 19, false) == UnitClass::Vcl);
  CHECK(classify_unit(Codec::AV1, 1, false) == UnitClass::ParameterSet);
  CHECK(classify_unit(Codec::AV1, 5, false) == UnitClass::Sei);
  CHECK(classify_unit(Codec::AV1, 2, false) == UnitClass::Aud);
  CHECK(classify_unit(Codec::AV1, 6, false) == UnitClass::Vcl);
  CHECK(classify_unit(Codec::Unknown, 1, false) == UnitClass::Other);
}

TEST_CASE("typing SEI in the filter lists only SEI units", "[app][unitlist]") {  // acceptance test 1
  ovt::app::testing::qt_app();
  UnitListModel model;
  model.setHandle(StreamIndexHandle::adopt(build_index(sei_stream())));
  const QSignalSpy filter(&model, &UnitListModel::filterChanged);

  model.set_text(QStringLiteral("SEI"));
  CHECK(filter.count() == 1);
  CHECK(units_of(model) == std::vector<int>{3, 6});
  for (int r = 0; r < model.rowCount(); ++r) {
    CHECK(cell(model, r, UnitListModel::ColType) == QStringLiteral("SEI"));
  }
  model.set_text(QStringLiteral("sei"));  // case-insensitive
  CHECK(units_of(model) == std::vector<int>{3, 6});
  model.set_text(QStringLiteral("SLICE"));
  CHECK(units_of(model) == std::vector<int>{4, 7, 9});
  model.set_text(QStringLiteral("nothing"));
  CHECK(model.rowCount() == 0);
  model.set_text(QString());
  CHECK(model.rowCount() == 10);
}

TEST_CASE("the kind chips filter VCL, non-VCL, SEI and units with diagnostics", "[app][unitlist]") {
  ovt::app::testing::qt_app();
  UnitListModel model;
  model.setHandle(StreamIndexHandle::adopt(build_index(sei_stream())));
  model.set_kind(UnitListModel::KindVcl);
  CHECK(units_of(model) == std::vector<int>{4, 7, 9});
  model.set_kind(UnitListModel::KindNonVcl);
  CHECK(units_of(model) == std::vector<int>{0, 1, 2, 3, 5, 6, 8});
  model.set_kind(UnitListModel::KindSei);
  CHECK(units_of(model) == std::vector<int>{3, 6});
  model.set_kind(UnitListModel::KindErrors);
  CHECK(units_of(model) == std::vector<int>{6, 9});
  model.set_text(QStringLiteral("slice"));  // chips and text combine
  CHECK(units_of(model) == std::vector<int>{9});
  model.set_kind(UnitListModel::KindAll);
  model.set_text(QString());
  CHECK(model.rowCount() == 10);
}

TEST_CASE("sorting is stable and keeps the decode order of equal rows", "[app][unitlist]") {
  ovt::app::testing::qt_app();
  UnitListModel model;
  model.setHandle(StreamIndexHandle::adopt(build_index(sei_stream())));

  model.sort(UnitListModel::ColType, Qt::AscendingOrder);
  // AUD x3, PPS, SEI x2, SLICE x2, SLICE_IDR, SPS: "SLICE" < "SLICE_IDR".
  CHECK(units_of(model) == std::vector<int>{0, 5, 8, 2, 3, 6, 7, 9, 4, 1});
  model.sort(UnitListModel::ColType, Qt::DescendingOrder);
  CHECK(units_of(model) == std::vector<int>{1, 4, 7, 9, 3, 6, 2, 0, 5, 8});

  model.sort(UnitListModel::ColSize, Qt::AscendingOrder);  // sizes shrink with the index
  CHECK(units_of(model) == std::vector<int>{9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
  model.sort(UnitListModel::ColNumber, Qt::DescendingOrder);
  CHECK(units_of(model) == std::vector<int>{9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
  model.sort(UnitListModel::ColNumber, Qt::AscendingOrder);
  CHECK(model.unitAt(0) == 0);

  model.sort(UnitListModel::ColFrame, Qt::AscendingOrder);  // -1 (non-VCL) first, frames 0, 1, 2 last
  CHECK(units_of(model) == std::vector<int>{0, 1, 2, 3, 5, 6, 8, 4, 7, 9});
  model.sort(UnitListModel::ColFlags, Qt::DescendingOrder);  // 2 diagnostics, 1, then 0 in decode order
  CHECK(units_of(model) == std::vector<int>{9, 6, 0, 1, 2, 3, 4, 5, 7, 8});

  const QSignalSpy sorted(&model, &UnitListModel::sortChanged);
  model.toggleSort(UnitListModel::ColFlags);
  CHECK(model.sort_column() == UnitListModel::ColFlags);
  CHECK_FALSE(model.sort_descending());
  model.toggleSort(UnitListModel::ColType);
  CHECK(model.sort_column() == UnitListModel::ColType);
  CHECK(sorted.count() == 2);
  model.sort(UnitListModel::ColNumber, Qt::AscendingOrder);
}

TEST_CASE("sorting applies on top of the filter and rowOfUnit follows it", "[app][unitlist]") {
  ovt::app::testing::qt_app();
  UnitListModel model;
  model.setHandle(StreamIndexHandle::adopt(build_index(sei_stream())));
  model.set_kind(UnitListModel::KindVcl);
  model.sort(UnitListModel::ColNumber, Qt::DescendingOrder);
  CHECK(units_of(model) == std::vector<int>{9, 7, 4});
  CHECK(model.rowOfUnit(7) == 1);
  CHECK(model.rowOfUnit(3) == -1);  // filtered out
  CHECK(model.rowOfUnit(-1) == -1);
  model.sort(UnitListModel::ColType, Qt::AscendingOrder);
  CHECK(model.rowOfUnit(4) == 2);
  CHECK(model.unitAt(99) == -1);
}

TEST_CASE("clicking selects the unit, double-clicking a VCL unit seeks its frame", "[app][unitlist]") {
  // acceptance test 2
  ovt::app::testing::qt_app();
  StreamIndexModel stream;
  stream.setIndex(StreamIndexHandle::adopt(build_index(sei_stream())));
  SelectionModel selection;
  selection.setIndex(stream.selectionIndex());

  UnitListModel model;
  model.set_stream_index(&stream);
  model.set_selection(&selection);
  REQUIRE(model.rowCount() == 10);

  model.select(3, QStringLiteral("units"));  // SEI
  CHECK(selection.unit_index() == 3);
  CHECK(selection.frame_decode_index() == -1);
  CHECK(selection.origin() == QStringLiteral("units"));

  model.activate(3, QStringLiteral("units"));  // double-click on non-VCL: only the unit
  CHECK(selection.unit_index() == 3);
  CHECK(selection.frame_decode_index() == -1);

  model.activate(7, QStringLiteral("units"));  // P frame: decode 1, display 2
  CHECK(selection.frame_decode_index() == 1);
  CHECK(selection.frame_display_index() == 2);
  CHECK(selection.unit_index() == 7);

  model.activate(9, QStringLiteral("units"));
  CHECK(selection.frame_decode_index() == 2);
  CHECK(selection.frame_display_index() == 1);

  model.activate(-1, QStringLiteral("units"));  // no such row: nothing changes
  CHECK(selection.frame_decode_index() == 2);
}

TEST_CASE("the model follows the StreamIndexModel and survives a missing index", "[app][unitlist]") {
  ovt::app::testing::qt_app();
  StreamIndexModel stream;
  UnitListModel model;
  model.set_stream_index(&stream);
  CHECK(model.rowCount() == 0);
  model.select(0, QStringLiteral("units"));  // no selection model and no rows: harmless
  model.activate(0, QStringLiteral("units"));

  const QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
  stream.setIndex(StreamIndexHandle::adopt(build_index(sei_stream())));
  CHECK(reset.count() >= 1);
  CHECK(model.rowCount() == 10);
  stream.setIndex(nullptr);
  CHECK(model.rowCount() == 0);
  CHECK(model.unit_count() == 0);
}

TEST_CASE("rowText copies a row as tab-separated cells", "[app][unitlist]") {
  ovt::app::testing::qt_app();
  UnitListModel model;
  model.setHandle(StreamIndexHandle::adopt(build_index(sei_stream())));
  CHECK(model.rowText(4) == QStringLiteral("4\tSLICE_IDR\t0/0\t%1\t%2\t0/0\t\tRAP")
                                .arg(cell(model, 4, UnitListModel::ColOffset), cell(model, 4, UnitListModel::ColSize)));
  CHECK(model.rowText(99).isEmpty());
}
