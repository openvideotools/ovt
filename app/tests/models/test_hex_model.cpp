// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of HexModel (WI-1.458; chapter 11 §11.3.5). Acceptance test 1 (EPB mapping) and 2 (search = start codes) are
// here; acceptance test 3 (Ctrl+B and B) is app/tests/qml/tst_hex.qml. The expected values are worked out by hand from
// the normative algorithm of §11.3.5, not computed with the code under test.

#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "models/hex_model.h"
#include "models/jobs_model.h"
#include "models/selection_model.h"
#include "models/stream_index_adapter.h"
#include "ovt_model_generated.h"
#include "qt_test_app.h"
#include "synthetic_index.h"

namespace {

using namespace ovt::app;  // NOLINT(google-build-using-namespace): test file
using namespace std::chrono_literals;

// An index with one unit: a 4-byte start code at `payload - 4`, the payload `length` bytes at `payload`.
std::shared_ptr<const StreamIndexHandle> one_unit(std::uint64_t payload, std::uint64_t length,
                                                  std::vector<std::uint32_t> epb) {
  ovt::fb::StreamIndexT index;
  index.codec = ovt::fb::Codec::H264;
  index.schema_version = 1;
  auto unit = std::make_unique<ovt::fb::UnitT>();
  unit->index = 0;
  unit->prefix_span = std::make_unique<ovt::fb::ByteSpan>(payload - 4, 4);
  unit->file_span = std::make_unique<ovt::fb::ByteSpan>(payload, length);
  unit->type_name = "SLICE";
  unit->epb_positions = std::move(epb);
  index.units.push_back(std::move(unit));
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(ovt::fb::StreamIndex::Pack(fbb, &index));
  const auto bytes = fbb.GetBufferSpan();
  return StreamIndexHandle::adopt({bytes.begin(), bytes.end()});
}

std::shared_ptr<MemoryByteSource> ramp(std::size_t size) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::uint8_t>(i * 7U + 3U);
  }
  return std::make_shared<MemoryByteSource>(std::move(bytes));
}

QVariant cell(const HexModel& model, std::uint64_t offset, int role) {
  return model.data(model.index(model.rowOfOffset(static_cast<qlonglong>(offset)),
                                HexModel::ColByte0 + static_cast<int>(offset % 16)),
                    role);
}

}  // namespace

TEST_CASE("HexModel pages the file in and never needs all of it", "[fast][gui][hex]") {
  HexModel model;
  model.setSource(ramp(200000));
  REQUIRE(model.rowCount() == 12500);  // ceil(200000 / 16)
  REQUIRE(model.columnCount() == HexModel::ColumnCount);
  // The first read finds the page missing: a grey cell, and the page is asked for.
  REQUIRE_FALSE(cell(model, 70000, HexModel::LoadedRole).toBool());
  REQUIRE(cell(model, 70000, HexModel::ByteRole).toInt() == -1);
  QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
  REQUIRE(ovt::app::testing::spin_until([&] { return changed.count() > 0; }, 2000ms));
  REQUIRE(cell(model, 70000, HexModel::LoadedRole).toBool());
  REQUIRE(cell(model, 70000, HexModel::ByteRole).toInt() == static_cast<int>((70000U * 7U + 3U) & 0xFFU));
  REQUIRE(cell(model, 70000, Qt::DisplayRole).toString().size() == 2);
  // Offsets are 8 hex digits, the ASCII column is 16 characters.
  REQUIRE(model.data(model.index(1, HexModel::ColOffset)).toString() == QStringLiteral("00000010"));
  REQUIRE(model.data(model.index(model.rowOfOffset(70000), HexModel::ColAscii)).toString().size() == 16);
}

TEST_CASE("Hex acceptance 1: a field over bits 17..54 with an EPB at raw offset 3 highlights the oracle bytes",
          "[fast][gui][hex]") {
  // Unit payload at 1000, EPB at raw offset 3 (file offset 1003). Hand mapping (§11.3.5): bit 17 is RBSP byte 2 bit 1
  // → raw 2 (the EPB at 3 is not <= 2); bit 54 is RBSP byte 6 bit 6 → raw 6, the EPB 3 <= 6 → 7. RBSP bytes 3, 4, 5
  // sit at raw 4, 5, 6 (the EPB is skipped visually). So: 1002 holds bits 1..7 (0x7F), 1004..1006 whole, 1007 bits
  // 0..6 (0xFE); 1003 (the EPB) is dim and not highlighted; 1001 and 1008 are outside.
  auto handle = one_unit(1000, 30, {3});
  REQUIRE(handle != nullptr);
  SelectionModel selection;
  selection.setIndex(std::make_shared<StreamIndexAdapter>(handle));
  HexModel model;
  model.setSource(ramp(1100));
  model.setHandle(handle);
  model.set_selection(&selection);
  selection.selectField(0, QStringLiteral("slice_header.first_mb_in_slice"), 17, 38, QStringLiteral("test"));

  const std::vector<std::pair<std::uint64_t, int>> oracle = {{1001, 0},    {1002, 0x7F}, {1003, 0},    {1004, 0xFF},
                                                             {1005, 0xFF}, {1006, 0xFF}, {1007, 0xFE}, {1008, 0}};
  for (const auto& [offset, mask] : oracle) {
    INFO("offset " << offset);
    REQUIRE(cell(model, offset, HexModel::HighlightRole).toInt() == mask);
  }
  REQUIRE(cell(model, 1003, HexModel::EpbRole).toBool());
  REQUIRE_FALSE(cell(model, 1004, HexModel::EpbRole).toBool());
  // The ribbon starts one byte before the field and boxes the same bits.
  const QVariantList ribbon = model.ribbon();
  REQUIRE(ribbon.size() == 8);
  REQUIRE(ribbon.at(0).toMap().value(QStringLiteral("offset")).toInt() == 1001);
  REQUIRE(ribbon.at(1).toMap().value(QStringLiteral("mask")).toInt() == 0x7F);
  REQUIRE(ribbon.at(2).toMap().value(QStringLiteral("epb")).toBool());
  REQUIRE(model.ribbon_field() == QStringLiteral("slice_header.first_mb_in_slice"));
}

TEST_CASE("Hex bands tint units and their start codes", "[fast][gui][hex]") {
  const auto bytes = ovt::app::testing::build_index(ovt::app::testing::gop60(3, 1000), 1, 1000);
  auto handle = StreamIndexHandle::adopt(bytes);
  REQUIRE(handle != nullptr);
  HexModel model;
  model.setSource(ramp(20000));
  model.setHandle(handle);
  const auto* units = handle->index().units();
  REQUIRE(units->size() == 6);
  const auto prefix = units->Get(0)->prefix_span()->offset();
  REQUIRE(prefix == 100);
  REQUIRE(model.bandTintAt(99) == HexModel::TintNone);
  REQUIRE(model.bandTintAt(100) == HexModel::TintUnitPrefix);
  REQUIRE(model.bandTintAt(103) == HexModel::TintUnitPrefix);
  REQUIRE(model.bandTintAt(104) == HexModel::TintUnitA);
  const auto second = static_cast<qlonglong>(units->Get(1)->prefix_span()->offset());
  REQUIRE(model.bandTintAt(second) == HexModel::TintUnitPrefix);
  REQUIRE(model.bandTintAt(second + 4) == HexModel::TintUnitB);
}

TEST_CASE("Hex acceptance 2: searching 00 00 01 finds the start codes of the index", "[fast][gui][hex]") {
  const auto bytes = ovt::app::testing::build_index(ovt::app::testing::gop60(8, 1000), 1, 1000);
  auto handle = StreamIndexHandle::adopt(bytes);
  REQUIRE(handle != nullptr);
  // An Annex B file: 0xAB filler, `00 00 00 01` at every prefix.
  std::vector<std::uint8_t> file(200000, 0xAB);
  std::vector<std::uint64_t> expected;
  const auto* units = handle->index().units();
  for (flatbuffers::uoffset_t i = 0; i < units->size(); ++i) {
    const auto at = units->Get(i)->prefix_span()->offset();
    file.at(at) = 0;
    file.at(at + 1) = 0;
    file.at(at + 2) = 0;
    file.at(at + 3) = 1;
    expected.push_back(at + 1);  // 00 00 01 starts at the second zero of a 4-byte start code
  }
  REQUIRE(expected.size() == 16);

  SECTION("without a job model (inline)") {
    HexModel model;
    model.setSource(std::make_shared<MemoryByteSource>(file));
    REQUIRE(model.search(QStringLiteral("00 00 01")).isEmpty());
    REQUIRE(model.matches() == expected);
    REQUIRE(model.match_count() == 16);
    REQUIRE_FALSE(model.searching());
  }
  SECTION("as a job, with wildcards, and next / previous select the match") {
    JobsModel jobs(2);
    SelectionModel selection;
    HexModel model;
    model.setSource(std::make_shared<MemoryByteSource>(file));
    model.set_jobs(&jobs);
    model.set_selection(&selection);
    REQUIRE(model.search(QStringLiteral("00 ?? 01")).isEmpty());
    REQUIRE(model.searching());
    REQUIRE(ovt::app::testing::spin_until([&] { return !model.searching(); }, 5000ms));
    REQUIRE(model.matches() == expected);
    REQUIRE(model.findNext());
    REQUIRE(selection.byte_offset() == static_cast<qlonglong>(expected[0]));
    REQUIRE(selection.byte_length() == 3);
    REQUIRE(model.findNext());
    REQUIRE(selection.byte_offset() == static_cast<qlonglong>(expected[1]));
    REQUIRE(model.findPrevious());
    REQUIRE(model.findPrevious());  // wraps to the last match
    REQUIRE(selection.byte_offset() == static_cast<qlonglong>(expected.back()));
  }
}

TEST_CASE("Hex search patterns and chunk boundaries", "[fast][gui][hex]") {
  QString error;
  REQUIRE_FALSE(parse_hex_pattern(QString(), &error).has_value());
  REQUIRE_FALSE(parse_hex_pattern(QStringLiteral("0"), &error).has_value());
  REQUIRE_FALSE(parse_hex_pattern(QStringLiteral("zz"), &error).has_value());
  REQUIRE_FALSE(parse_hex_pattern(QStringLiteral("?? ??"), &error).has_value());
  const auto contiguous = ovt::app::testing::must(parse_hex_pattern(QStringLiteral("0000 01")));
  REQUIRE(contiguous.bytes == std::vector<std::uint8_t>{0, 0, 1});

  // A source is read in 1 MiB chunks; a match across a chunk edge (4 MiB - 1) is still found, once.
  struct ReadOnly final : ByteSource {
    std::vector<std::uint8_t> data;
    [[nodiscard]] std::uint64_t size() const override { return data.size(); }
    [[nodiscard]] std::size_t read(std::uint64_t offset, std::uint8_t* dst, std::size_t len) const override {
      if (offset >= data.size()) {
        return 0;
      }
      const std::size_t n = std::min<std::uint64_t>(len, data.size() - offset);
      std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), n, dst);
      return n;
    }
  };
  ReadOnly source;
  source.data.assign((4U << 20U) + 100, 0x11);
  const std::uint64_t edge = (4U << 20U) - 1;
  source.data[edge] = 0xDE;
  source.data[edge + 1] = 0xAD;
  source.data[edge + 2] = 0xBE;
  const auto pattern = ovt::app::testing::must(parse_hex_pattern(QStringLiteral("DE ad ?? ")));
  REQUIRE(hex_find_all(source, pattern, 0, source.size(), 10) == std::vector<std::uint64_t>{edge});
  // The scope [begin, end) is respected and max_results truncates.
  REQUIRE(hex_find_all(source, pattern, edge + 1, source.size(), 10).empty());
  const auto filler = ovt::app::testing::must(parse_hex_pattern(QStringLiteral("11")));
  REQUIRE(hex_find_all(source, filler, 0, source.size(), 5).size() == 5);
}

TEST_CASE("Hex Go to: 0x, decimal, unit:N and frame:N", "[fast][gui][hex]") {
  const auto bytes = ovt::app::testing::build_index(ovt::app::testing::gop60(3, 1000), 1, 1000);
  auto handle = StreamIndexHandle::adopt(bytes);
  REQUIRE(handle != nullptr);
  SelectionModel selection;
  selection.setIndex(std::make_shared<StreamIndexAdapter>(handle));
  HexModel model;
  model.setSource(ramp(20000));
  model.setHandle(handle);
  model.set_selection(&selection);
  const auto* units = handle->index().units();

  REQUIRE(ovt::app::testing::must(model.parseGoto(QStringLiteral("0x1A3")).offset) == 0x1A3);
  REQUIRE(ovt::app::testing::must(model.parseGoto(QStringLiteral("  4660 ")).offset) == 4660);
  REQUIRE(ovt::app::testing::must(model.parseGoto(QStringLiteral("@0x10")).offset) == 16);
  const HexGoto unit = model.parseGoto(QStringLiteral("unit:3"));
  REQUIRE(unit.unit == 3);
  REQUIRE(ovt::app::testing::must(unit.offset) == units->Get(3)->prefix_span()->offset());
  // Frame 1's first VCL unit is its slice (unit 3: AUD 2, slice 3).
  REQUIRE(model.parseGoto(QStringLiteral("frame:1")).unit == 3);
  for (const char* bad : {"", "banana", "0x", "unit:99", "frame:99", "99999999", "-4"}) {
    INFO(bad);
    const HexGoto result = model.parseGoto(QString::fromLatin1(bad));
    REQUIRE_FALSE(result.offset.has_value());
    REQUIRE_FALSE(result.error.isEmpty());
  }

  const QSignalSpy reveal(&model, &HexModel::revealRequested);
  REQUIRE(model.goTo(QStringLiteral("0x2A1")).isEmpty());
  REQUIRE(selection.byte_offset() == 0x2A1);
  REQUIRE(selection.byte_length() == 1);
  REQUIRE(reveal.count() == 1);
  REQUIRE(reveal.at(0).at(0).toInt() == 0x2A1 / 16);
  REQUIRE(model.goTo(QStringLiteral("unit:3")).isEmpty());
  REQUIRE(selection.unit_index() == 3);
  REQUIRE_FALSE(model.goTo(QStringLiteral("unit:99")).isEmpty());
  REQUIRE(selection.unit_index() == 3);  // a failed Go to changes nothing
}

TEST_CASE("Hex reverse lookup: byte → unit and the deepest field", "[fast][gui][hex]") {
  // Payload at 1000 with an EPB at raw 3. RBSP bytes: raw 0,1,2 → 0,1,2; raw 4 → 3; raw 5 → 4; ...
  auto handle = one_unit(1000, 30, {3});
  SelectionModel selection;
  selection.setIndex(std::make_shared<StreamIndexAdapter>(handle));
  HexModel model;
  model.setSource(ramp(1100));
  model.setHandle(handle);
  model.set_selection(&selection);
  // a: bits 0..15; b: bits 16..55; b.c nested inside b: bits 24..39.
  model.setFieldProvider([](int unit) {
    REQUIRE(unit == 0);
    return std::vector<HexField>{
        {QStringLiteral("b.c"), {24, 16}}, {QStringLiteral("a"), {0, 16}}, {QStringLiteral("b"), {16, 40}}};
  });

  auto path_at = [&](qlonglong offset) { return model.lookup(offset).value(QStringLiteral("fieldPath")).toString(); };
  REQUIRE(model.lookup(1000).value(QStringLiteral("unit")).toInt() == 0);
  REQUIRE(model.lookup(900).value(QStringLiteral("unit")).toInt() == -1);
  REQUIRE(path_at(1000) == QStringLiteral("a"));
  REQUIRE(path_at(1002) == QStringLiteral("b"));
  REQUIRE(path_at(1003).isEmpty());                 // the EPB belongs to no field
  REQUIRE(path_at(1004) == QStringLiteral("b.c"));  // RBSP byte 3 = bit 24
  REQUIRE(path_at(1007) == QStringLiteral("b"));    // RBSP byte 6 = bit 48, past b.c
  REQUIRE(path_at(1020).isEmpty());

  // Clicking a byte selects the field (and with it the unit); a drag selects the bytes.
  model.selectByteRange(1004, 1004);
  REQUIRE(selection.field_path() == QStringLiteral("b.c"));
  REQUIRE(selection.unit_index() == 0);
  model.selectByteRange(1010, 1012);
  REQUIRE(selection.field_path().isEmpty());
  REQUIRE(selection.byte_offset() == 1010);
  REQUIRE(selection.byte_length() == 3);
}

TEST_CASE("Hex scope restricts the rows to the unit, offsets stay absolute", "[fast][gui][hex]") {
  const auto bytes = ovt::app::testing::build_index(ovt::app::testing::gop60(3, 1000), 1, 1000);
  auto handle = StreamIndexHandle::adopt(bytes);
  SelectionModel selection;
  selection.setIndex(std::make_shared<StreamIndexAdapter>(handle));
  HexModel model;
  model.setSource(ramp(20000));
  model.setHandle(handle);
  model.set_selection(&selection);
  const int all_rows = model.rowCount();
  selection.selectUnit(1, QStringLiteral("test"));
  model.set_scope(HexModel::ScopeUnit);
  const auto* unit = handle->index().units()->Get(1);
  const auto begin = unit->prefix_span()->offset();
  REQUIRE(model.rowCount() < all_rows);
  REQUIRE(model.offsetOfRow(0) == static_cast<qlonglong>(begin / 16 * 16));
  REQUIRE(model.rowOfOffset(static_cast<qlonglong>(begin)) == 0);
  REQUIRE(model.rowOfOffset(5) == -1);
  model.set_scope(HexModel::ScopeFile);
  REQUIRE(model.rowCount() == all_rows);
}

TEST_CASE("Hex copies bytes as hex and as a C array", "[fast][gui][hex]") {
  HexModel model;
  model.setSource(std::make_shared<MemoryByteSource>(std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0xAF}));
  REQUIRE(model.copyHex(0, 4) == QStringLiteral("00 00 01 AF"));
  REQUIRE(model.copyCArray(1, 3) == QStringLiteral("{ 0x00, 0x01, 0xAF }"));
}

namespace {

// A source whose first read blocks until released, so a search job can be held while others run.
struct GatedSource final : ovt::app::ByteSource {
  std::vector<std::uint8_t> data;
  mutable std::atomic<int> reads{0};
  std::atomic<bool> released{false};
  [[nodiscard]] std::uint64_t size() const override { return data.size(); }
  [[nodiscard]] std::size_t read(std::uint64_t offset, std::uint8_t* dst, std::size_t len) const override {
    if (reads.fetch_add(1) == 0) {
      while (!released.load()) {
        std::this_thread::sleep_for(1ms);
      }
    }
    if (offset >= data.size()) {
      return 0;
    }
    const std::size_t n = std::min<std::uint64_t>(len, data.size() - offset);
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), n, dst);
    return n;
  }
};

}  // namespace

TEST_CASE("Hex search: an older search finishing late never replaces a newer one's matches", "[fast][gui][hex]") {
  JobsModel jobs(2);
  HexModel model;
  auto source = std::make_shared<GatedSource>();
  source->data = {0x11, 0x22, 0x11, 0x22, 0x22};
  const std::shared_ptr<GatedSource> gate = source;
  model.setSource(std::move(source));
  model.set_jobs(&jobs);
  REQUIRE(model.search(QStringLiteral("11")).isEmpty());  // A: its job is held on the first read
  REQUIRE(ovt::app::testing::spin_until([&] { return gate->reads.load() > 0; }, 5000ms));
  REQUIRE(model.search(QStringLiteral("22")).isEmpty());  // B replaces A
  REQUIRE(ovt::app::testing::spin_until([&] { return !model.searching(); }, 5000ms));
  REQUIRE(model.matches() == std::vector<std::uint64_t>{1, 3, 4});
  gate->released.store(true);
  REQUIRE(ovt::app::testing::spin_until([&] { return jobs.activeCount() == 0; }, 5000ms));
  QCoreApplication::processEvents();
  REQUIRE(model.matches() == std::vector<std::uint64_t>{1, 3, 4});
  REQUIRE_FALSE(model.searching());
}

TEST_CASE("Hex search: a model destroyed while its job runs is not touched by the job", "[fast][gui][hex]") {
  JobsModel jobs(2);
  auto source = std::make_shared<GatedSource>();
  source->data = {0x11, 0x22, 0x11};
  const std::shared_ptr<GatedSource> gate = source;
  auto model = std::make_unique<HexModel>();
  model->setSource(std::move(source));
  model->set_jobs(&jobs);
  REQUIRE(model->search(QStringLiteral("11")).isEmpty());
  REQUIRE(ovt::app::testing::spin_until([&] { return gate->reads.load() > 0; }, 5000ms));
  model.reset();
  gate->released.store(true);
  REQUIRE(ovt::app::testing::spin_until([&] { return jobs.activeCount() == 0; }, 5000ms));
  QCoreApplication::processEvents();
}
