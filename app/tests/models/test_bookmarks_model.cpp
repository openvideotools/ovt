// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of BookmarksModel (WI-2.487; chapter 11 §11.3.17).
//   AT1: a comment anchored to a field selects that field (Syntax: unit and path; Hex: the field's bytes) when clicked.
//   AT2: three bookmarks and `N` three times visit them in frame order (and wrap).
// Plus: storage in the project's bookmarks[] / comments[] (valid against ovtproj.schema.json), the other anchors,
// timeline markers, reorder / rename / colour, and the JSON / CSV export and the import.

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "models/bookmarks_model.h"
#include "models/selection_model.h"
#include "project.h"
#include "qt_test_app.h"

namespace {

using ovt::app::BitSpan;
using ovt::app::BookmarkRows;
using ovt::app::BookmarksModel;
using ovt::app::ByteSpan;
using ovt::app::FrameId;
using ovt::app::Project;
using ovt::app::SelectionIndex;
using ovt::app::SelectionModel;
using ovt::app::SelectionSample;
using ovt::app::SelectionUnit;

// Unit 0: a slice of frame decode 0; unit 1: a slice of frame decode 1. Frames 0..9 exist (display == decode).
class FakeIndex final : public SelectionIndex {
 public:
  [[nodiscard]] std::optional<SelectionUnit> unit(int i) const override {
    if (i == 0) {
      return SelectionUnit{{100, 50}, {}, {0, 0}, 0};
    }
    if (i == 1) {
      return SelectionUnit{{150, 50}, {}, {1, 1}, 0};
    }
    return std::nullopt;
  }
  [[nodiscard]] std::optional<FrameId> resolve_frame(FrameId f) const override {
    if (f.decode_index >= 0 && f.decode_index < 10) {
      return FrameId{f.decode_index, f.decode_index};
    }
    return std::nullopt;
  }
  [[nodiscard]] int first_vcl_unit(FrameId f) const override { return f.decode_index < 2 ? f.decode_index : -1; }
  [[nodiscard]] std::optional<SelectionSample> sample(int) const override { return std::nullopt; }
  [[nodiscard]] int unit_at_byte(std::uint64_t offset) const override {
    return offset >= 100 && offset < 150 ? 0 : (offset >= 150 && offset < 200 ? 1 : -1);
  }
};

struct Fixture {
  QTemporaryDir dir;
  Project project{dir.filePath(QStringLiteral("cfg"))};
  SelectionModel selection;
  BookmarksModel model;

  Fixture() {
    ovt::app::testing::qt_app();
    selection.setIndex(std::make_shared<FakeIndex>());
    selection.setSource(QStringLiteral("f1"), 0, QStringLiteral("test"));
    model.attach(&project, &selection);
  }

  void bookmark_frame(int decode) {
    selection.selectFrame(decode, -1, QStringLiteral("test"));
    REQUIRE(!model.addBookmark().isEmpty());
  }

  [[nodiscard]] QJsonObject read_json(const QString& path) const {
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(file.readAll()).object();
  }
};

}  // namespace

TEST_CASE("BookmarksModel: AT1 a comment anchored to a field selects it for Syntax and Hex", "[fast][gui][bookmarks]") {
  Fixture f;
  f.selection.setField(1, QStringLiteral("slice_header.first_mb_in_slice"), BitSpan{16, 8}, QStringLiteral("syntax"));
  const QString id = f.model.addComment(QStringLiteral("check this field"));
  REQUIRE(!id.isEmpty());

  f.selection.clear(QStringLiteral("test"));
  REQUIRE(f.selection.unit_index() < 0);
  REQUIRE(f.model.goToComment(id));

  CHECK(f.selection.unit_index() == 1);  // Syntax
  CHECK(f.selection.field_path() == QStringLiteral("slice_header.first_mb_in_slice"));
  CHECK(f.selection.byte_offset() == 152);  // Hex: unit 1 starts at 150, bit 16 is byte 2
  CHECK(f.selection.byte_length() == 1);
  CHECK(f.selection.origin() == QStringLiteral("bookmarks"));
}

TEST_CASE("BookmarksModel: AT2 N visits three bookmarks in frame order", "[fast][gui][bookmarks]") {
  Fixture f;
  // Added out of order: the cycle follows the frame, not the creation order.
  f.bookmark_frame(7);
  f.bookmark_frame(2);
  f.bookmark_frame(5);
  f.selection.clearSelection(QStringLiteral("test"));

  std::vector<int> visited;
  for (int i = 0; i < 4; ++i) {
    REQUIRE(f.model.nextBookmark());
    visited.push_back(f.selection.frame_decode_index());
  }
  CHECK(visited == std::vector<int>{2, 5, 7, 2});  // the fourth press wraps

  std::vector<int> back;
  for (int i = 0; i < 3; ++i) {
    REQUIRE(f.model.previousBookmark());
    back.push_back(f.selection.frame_decode_index());
  }
  CHECK(back == std::vector<int>{7, 5, 2});
}

TEST_CASE("BookmarksModel: bookmarks and comments live in the project and validate against the schema",
          "[fast][gui][bookmarks]") {
  Fixture f;
  f.bookmark_frame(3);
  CHECK(f.model.addBookmark() == f.model.addBookmark());  // the same frame is not bookmarked twice
  CHECK(f.model.bookmark_count() == 1);

  f.selection.selectFrame(4, -1, QStringLiteral("test"));
  REQUIRE(!f.model.addComment(QStringLiteral("a frame comment")).isEmpty());
  f.selection.selectBytes(120, 4, QStringLiteral("hex"));
  REQUIRE(!f.model.addComment(QStringLiteral("bytes")).isEmpty());
  REQUIRE(!f.model.addBoxComment(QStringLiteral("moov/trak[1]/stsz"), QStringLiteral("box")).isEmpty());
  CHECK(f.model.addComment(QStringLiteral("   ")).isEmpty());

  const QJsonArray bookmarks = f.project.section(QStringLiteral("bookmarks")).toArray();
  const QJsonArray comments = f.project.section(QStringLiteral("comments")).toArray();
  REQUIRE(bookmarks.size() == 1);
  REQUIRE(comments.size() == 3);
  CHECK(bookmarks.at(0).toObject().value(QStringLiteral("decode_index")).toInt() == 3);
  CHECK(comments.at(0).toObject().value(QStringLiteral("anchor")).toObject().contains(QStringLiteral("frame")));
  CHECK(comments.at(1).toObject().value(QStringLiteral("anchor")).toObject().contains(QStringLiteral("bytes")));
  CHECK(comments.at(2).toObject().value(QStringLiteral("anchor")).toObject().contains(QStringLiteral("box")));

  // The document written by the project passes the project schema (bookmarks[] and comments[] items).
  QString reason;
  CHECK(Project::validate(f.project.document(f.dir.path()), &reason));
  CHECK(reason.isEmpty());

  // A project that is reloaded brings the lists back.
  const QString path = f.dir.filePath(QStringLiteral("p.ovtproj"));
  REQUIRE(f.project.saveAs(path));
  Project second(f.dir.filePath(QStringLiteral("cfg2")));
  BookmarksModel other;
  other.attach(&second, nullptr);
  CHECK(other.bookmark_count() == 0);
  REQUIRE(second.load(path));
  CHECK(other.bookmark_count() == 1);
  CHECK(other.comment_count() == 3);
}

TEST_CASE("BookmarksModel: block, frame and byte anchors navigate; a box anchor asks the Container panel",
          "[fast][gui][bookmarks]") {
  Fixture f;
  f.selection.selectFrame(4, -1, QStringLiteral("test"));
  f.selection.selectRect(QRect(8, 16, 32, 32), QStringLiteral("picture"));
  const QString block = f.model.addComment(QStringLiteral("block"));
  f.selection.clearSelection(QStringLiteral("test"));
  REQUIRE(f.model.goToComment(block));
  CHECK(f.selection.frame_decode_index() == 4);
  CHECK(f.selection.selection_rect() == QRect(8, 16, 32, 32));

  f.selection.selectBytes(160, 5, QStringLiteral("hex"));
  const QString bytes = f.model.addComment(QStringLiteral("bytes"));
  f.selection.clearSelection(QStringLiteral("test"));
  REQUIRE(f.model.goToComment(bytes));
  CHECK(f.selection.byte_offset() == 160);
  CHECK(f.selection.byte_length() == 5);

  const QString box = f.model.addBoxComment(QStringLiteral("moov/trak[1]/stsz"), QStringLiteral("box"));
  QSignalSpy spy(&f.model, &BookmarksModel::boxRequested);
  REQUIRE(f.model.goToComment(box));
  REQUIRE(spy.count() == 1);
  CHECK(spy.takeFirst().at(0).toString() == QStringLiteral("moov/trak[1]/stsz"));
}

TEST_CASE("BookmarksModel: timeline markers, rename, colour, reorder and delete", "[fast][gui][bookmarks]") {
  Fixture f;
  CHECK(f.model.addBookmark().isEmpty());  // nothing selected yet
  f.bookmark_frame(6);
  f.bookmark_frame(1);
  const QVariantList markers = f.model.markers();
  REQUIRE(markers.size() == 2);
  CHECK(markers.at(0).toMap().value(QStringLiteral("decodeIndex")).toInt() == 1);  // decode order
  CHECK(markers.at(1).toMap().value(QStringLiteral("decodeIndex")).toInt() == 6);

  const auto* rows = static_cast<BookmarkRows*>(f.model.bookmarks());
  const auto id_at = [rows](int row) { return rows->data(rows->index(row), BookmarkRows::IdRole).toString(); };
  const QString first = id_at(0);
  REQUIRE(f.model.renameBookmark(first, QStringLiteral("intro")));
  REQUIRE(f.model.setBookmarkColor(first, QStringLiteral("#112233")));
  CHECK_FALSE(f.model.setBookmarkColor(first, QStringLiteral("red")));  // the schema's pattern
  REQUIRE(f.model.moveBookmark(0, 1));
  CHECK(rows->data(rows->index(1), BookmarkRows::NameRole).toString() == QStringLiteral("intro"));
  CHECK(rows->data(rows->index(1), BookmarkRows::ColorRole).toString() == QStringLiteral("#112233"));
  CHECK_FALSE(f.model.moveBookmark(0, 5));

  REQUIRE(f.model.removeBookmark(first));
  CHECK(f.model.bookmark_count() == 1);
  CHECK_FALSE(f.model.removeBookmark(first));
}

TEST_CASE("BookmarksModel: export JSON and CSV, import merges with fresh ids", "[fast][gui][bookmarks]") {
  Fixture f;
  f.bookmark_frame(2);
  f.selection.selectFrame(2, -1, QStringLiteral("test"));
  REQUIRE(!f.model.addComment(QStringLiteral("needs, a \"quote\"")).isEmpty());

  const QString json = f.dir.filePath(QStringLiteral("out.json"));
  const QString csv = f.dir.filePath(QStringLiteral("out.csv"));
  REQUIRE(f.model.exportJson(json));
  REQUIRE(f.model.exportCsv(csv));
  CHECK(f.read_json(json).value(QStringLiteral("bookmarks")).toArray().size() == 1);

  QFile csv_file(csv);
  REQUIRE(csv_file.open(QIODevice::ReadOnly));
  const QString text = QString::fromUtf8(csv_file.readAll());
  CHECK(text.startsWith(QStringLiteral("id,file,decode_index,name,color,created\n")));
  CHECK(text.contains(QStringLiteral("\"needs, a \"\"quote\"\"\"")));  // RFC 4180 quoting

  // Importing the file into the same project adds both entries again, under new ids.
  CHECK(f.model.importJson(json) == 2);
  CHECK(f.model.bookmark_count() == 2);
  CHECK(f.model.comment_count() == 2);
  const QJsonArray bookmarks = f.project.section(QStringLiteral("bookmarks")).toArray();
  CHECK(bookmarks.at(0).toObject().value(QStringLiteral("id")) !=
        bookmarks.at(1).toObject().value(QStringLiteral("id")));
  CHECK(f.model.importJson(f.dir.filePath(QStringLiteral("missing.json"))) == -1);
}
