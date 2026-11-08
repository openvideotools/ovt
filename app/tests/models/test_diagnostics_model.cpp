// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of DiagnosticsModel (WI-1.453; chapter 11 §11.3.16). Acceptance tests 2 and 3 are asserted against a fake
// NavigationSink, which SelectionModel (WI-1.452) will implement (SPEC-ASSUMPTION #137).

#include <catch2/catch_test_macros.hpp>

#include <QModelIndex>
#include <QObject>
#include <QPoint>
#include <QString>

#include <cstdint>
#include <vector>

#include "models/diagnostics_model.h"
#include "qt_test_app.h"

namespace {

using ovt::app::DiagnosticEntry;
using ovt::app::DiagnosticLocation;
using ovt::app::DiagnosticsFilterModel;
using ovt::app::DiagnosticsModel;
using ovt::app::DiagSeverity;
using ovt::app::NavigationSink;

struct RecordingSink : NavigationSink {
  QString last;
  int unit_index = -1;
  std::uint64_t bit_offset = 0;
  std::uint32_t bit_length = 0;
  int decode_index = -1;
  int display_index = -1;
  std::uint32_t block_id = 0;
  QPoint probe{-1, -1};
  QString path;
  std::uint64_t offset = 0;
  QString origin;

  void showUnitBits(int unit, std::uint64_t off, std::uint32_t len, const QString& o) override {
    last = QStringLiteral("unit");
    unit_index = unit;
    bit_offset = off;
    bit_length = len;
    origin = o;
  }
  void showBox(const QString& p, const QString& o) override {
    last = QStringLiteral("box");
    path = p;
    origin = o;
  }
  void showFrameBlock(int decode, int display, std::uint32_t block, const QString& o) override {
    last = QStringLiteral("block");
    decode_index = decode;
    display_index = display;
    block_id = block;
    origin = o;
  }
  void showFrameProbe(int decode, int display, QPoint p, const QString& o) override {
    last = QStringLiteral("probe");
    decode_index = decode;
    display_index = display;
    probe = p;
    origin = o;
  }
  void showFileOffset(std::uint64_t off, const QString& o) override {
    last = QStringLiteral("offset");
    offset = off;
    origin = o;
  }
};

DiagnosticEntry entry(DiagSeverity severity, const char* code, const char* message) {
  DiagnosticEntry e;
  e.severity = severity;
  e.code = QString::fromLatin1(code);
  e.message = QString::fromLatin1(message);
  return e;
}

DiagnosticEntry unit_entry() {
  DiagnosticEntry e = entry(DiagSeverity::Error, "SYN-H265-0007", "bad value");
  e.spec_ref = QStringLiteral("H.265 7.3.2.2.1");
  e.location.kind = DiagnosticLocation::Kind::UnitBits;
  e.location.unit_index = 412;
  e.location.bit_offset = 102;
  e.location.bit_length = 6;
  return e;
}

DiagnosticEntry diff_entry() {
  DiagnosticEntry e = entry(DiagSeverity::Info, "VQ-DIFF-0001", "first differing sample");
  e.location.kind = DiagnosticLocation::Kind::FrameProbe;
  e.location.decode_index = 40;
  e.location.display_index = 37;
  e.location.probe = QPoint(640, 360);
  return e;
}

}  // namespace

TEST_CASE("Diagnostics: double-click on an entry with a BitSpan sets unit and bit range", "[fast][gui][diagnostics]") {
  ovt::app::testing::qt_app();
  DiagnosticsModel model;
  RecordingSink sink;
  model.setNavigationSink(&sink);
  model.setSource(QStringLiteral("f0/index"), {unit_entry()});
  REQUIRE(model.activate(0));
  CHECK(sink.last == QStringLiteral("unit"));
  CHECK(sink.unit_index == 412);
  CHECK(sink.bit_offset == 102);
  CHECK(sink.bit_length == 6);
  CHECK(sink.origin == QStringLiteral("diagnostics"));
}

TEST_CASE("Diagnostics: VQ-DIFF-0001 sets the frame and pins the probe", "[fast][gui][diagnostics]") {
  DiagnosticsModel model;
  RecordingSink sink;
  model.setNavigationSink(&sink);
  model.setSource(QStringLiteral("metrics"), {diff_entry()});
  REQUIRE(model.activate(0));
  CHECK(sink.last == QStringLiteral("probe"));
  CHECK(sink.decode_index == 40);
  CHECK(sink.display_index == 37);
  CHECK(sink.probe == QPoint(640, 360));
}

TEST_CASE("Diagnostics: box, block and file offset locations navigate; no location does not",
          "[fast][gui][diagnostics]") {
  DiagnosticsModel model;
  RecordingSink sink;
  model.setNavigationSink(&sink);
  DiagnosticEntry box = entry(DiagSeverity::Warning, "CNT-MP4-0012", "stsz overruns");
  box.location.kind = DiagnosticLocation::Kind::Box;
  box.location.box_path = QStringLiteral("moov/trak[1]/stsz");
  DiagnosticEntry block = entry(DiagSeverity::Warning, "DEC-H264-0003", "block");
  block.location.kind = DiagnosticLocation::Kind::FrameBlock;
  block.location.decode_index = 1203;
  block.location.display_index = 1200;
  block.location.block_id = 37;
  DiagnosticEntry offset = entry(DiagSeverity::Warning, "CNT-MP4-0001", "trailing bytes");
  offset.location.kind = DiagnosticLocation::Kind::FileOffset;
  offset.location.file_offset = 0x1f4;
  model.setSource(QStringLiteral("f0"), {box, block, offset, entry(DiagSeverity::Info, "ANA-0001", "no location")});

  CHECK(model.data(model.index(0, DiagnosticsModel::LocationColumn)).toString() ==
        QStringLiteral("box moov/trak[1]/stsz"));
  CHECK(model.data(model.index(1, DiagnosticsModel::LocationColumn)).toString() ==
        QStringLiteral("frame #1203 block 37"));
  CHECK(model.data(model.index(2, DiagnosticsModel::LocationColumn)).toString() == QStringLiteral("file offset 0x1f4"));
  CHECK(model.data(model.index(0, 0), DiagnosticsModel::NavigableRole).toBool());
  CHECK_FALSE(model.data(model.index(3, 0), DiagnosticsModel::NavigableRole).toBool());

  REQUIRE(model.activate(0));
  CHECK(sink.path == QStringLiteral("moov/trak[1]/stsz"));
  REQUIRE(model.activate(1));
  CHECK(sink.block_id == 37);
  CHECK(sink.decode_index == 1203);
  REQUIRE(model.activate(2));
  CHECK(sink.offset == 0x1f4);
  CHECK_FALSE(model.activate(3));
  CHECK_FALSE(model.activate(99));
}

TEST_CASE("Diagnostics: sources merge, counts follow, and a source is replaced as a whole",
          "[fast][gui][diagnostics]") {
  DiagnosticsModel model;
  int counts = 0;
  QObject::connect(&model, &DiagnosticsModel::countsChanged, &model, [&] { ++counts; });
  model.setSource(QStringLiteral("a"), {unit_entry(), entry(DiagSeverity::Warning, "SYN-H264-0103", "gap")});
  model.setSource(QStringLiteral("b"), {diff_entry(), entry(DiagSeverity::Fatal, "CNT-MP4-0100", "cannot index")});
  CHECK(model.rowCount() == 4);
  CHECK(model.errorCount() == 2);  // Error and Fatal
  CHECK(model.warningCount() == 1);
  model.setSource(QStringLiteral("a"), {});
  CHECK(model.rowCount() == 2);
  CHECK(model.errorCount() == 1);
  CHECK(model.warningCount() == 0);
  model.removeSource(QStringLiteral("b"));
  CHECK(model.rowCount() == 0);
  CHECK(counts >= 3);
  model.addGui(1, QStringLiteral("GUI-JOB-0001"), QStringLiteral("job X did not stop within 2 s; detached"));
  REQUIRE(model.rowCount() == 1);
  CHECK(model.data(model.index(0, DiagnosticsModel::CodeColumn)).toString() == QStringLiteral("GUI-JOB-0001"));
  CHECK(model.data(model.index(0, 0), DiagnosticsModel::ModuleRole).toString() == QStringLiteral("GUI"));
}

TEST_CASE("Diagnostics: more than 10 000 entries drop the oldest and say how many", "[fast][gui][diagnostics]") {
  DiagnosticsModel model;
  std::vector<DiagnosticEntry> many;
  many.reserve(10'005);
  for (int i = 0; i < 10'005; ++i) {
    many.push_back(entry(DiagSeverity::Info, "SYN-H264-0001", QByteArray::number(i).constData()));
  }
  model.setSource(QStringLiteral("big"), std::move(many));
  CHECK(model.rowCount() == 10'000);
  CHECK(model.droppedCount() == 5);
  CHECK(model.data(model.index(0, 0), DiagnosticsModel::MessageRole).toString() == QStringLiteral("5"));
  model.addGui(2, QStringLiteral("GUI-PROJ-0003"), QStringLiteral("file not found: x"));
  CHECK(model.rowCount() == 10'000);
  CHECK(model.droppedCount() == 6);
}

TEST_CASE("Diagnostics: severity, module and text filters; activate maps proxy rows", "[fast][gui][diagnostics]") {
  DiagnosticsModel model;
  RecordingSink sink;
  model.setNavigationSink(&sink);
  model.setSource(QStringLiteral("a"), {entry(DiagSeverity::Info, "ANA-0001", "note"), unit_entry(),
                                        entry(DiagSeverity::Warning, "SYN-H264-0103", "frame_num gap"), diff_entry()});
  auto* view = qobject_cast<DiagnosticsFilterModel*>(model.filtered());
  REQUIRE(view != nullptr);
  CHECK(view->rowCount() == 4);
  view->setMinSeverity(static_cast<int>(DiagSeverity::Warning));
  CHECK(view->rowCount() == 2);
  view->setModule(QStringLiteral("SYN"));
  CHECK(view->rowCount() == 2);  // SYN-H265 error and SYN-H264 warning
  view->setText(QStringLiteral("GAP"));
  REQUIRE(view->rowCount() == 1);
  CHECK(view->rowText(0) == QStringLiteral("WARNING SYN-H264-0103 frame_num gap"));
  view->setText(QString());
  REQUIRE(view->rowCount() == 2);
  REQUIRE(view->activate(0));  // the first visible row is the unit entry
  CHECK(sink.unit_index == 412);
  CHECK(view->allText().startsWith(
      QStringLiteral("ERROR SYN-H265-0007 bad value (unit #412 bit 102) [H.265 7.3.2.2.1]")));
}
