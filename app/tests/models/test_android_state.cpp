// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of AndroidState (WI-A.455; chapter 11 §11.5.7): the saved-state document round-trips the project files,
// SelectionState, ViewState, layout and panel state into a fresh set of models (what a recreated Activity's new QML
// engine has), stays at most 100 KB and is byte-identical when captured twice. The Android half (onSaveInstanceState,
// "Don't keep activities") is StateRestoreTest.kt.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "dock/dock_model.h"
#include "models/qt_test_app.h"
#include "models/selection_model.h"
#include "picture_controller.h"
#include "platform/android_state.h"
#include "project.h"

using Catch::Matchers::WithinAbs;
using ovt::app::AndroidState;
using ovt::app::DockModel;
using ovt::app::FileRole;
using ovt::app::PictureController;
using ovt::app::Project;
using ovt::app::SelectionModel;

namespace {

// The models of one QML engine.
struct Models {
  explicit Models(const QString& config_dir) : project(config_dir) {
    picture.set_selection(&selection);
    picture.set_viewport_width(800);
    picture.set_viewport_height(600);
    state.setSelection(&selection);
    state.setDock(&dock);
    state.setProject(&project);
    state.setPicture(&picture);
  }
  SelectionModel selection;
  DockModel dock{QString(), nullptr};  // in memory only
  Project project;
  PictureController picture;
  AndroidState state;
};

}  // namespace

TEST_CASE("the saved state restores frame, selection, view, layout and panel state into fresh models",
          "[fast][gui][android]") {
  ovt::app::testing::qt_app();
  const QTemporaryDir dir;
  QString saved;
  {
    Models before(dir.filePath(QStringLiteral("cfg1")));
    // Two files; the first one removed, so the second keeps id f2 (restored files are numbered afresh).
    const QString gone = before.project.addFile(dir.filePath(QStringLiteral("a.h264")), FileRole::Single);
    const QString id = before.project.addFile(QStringLiteral("content://provider/doc/b.mp4"), FileRole::Single, 1);
    REQUIRE(before.project.removeFile(gone));
    before.project.setActiveFile(id);
    before.selection.setSource(id, 1, QStringLiteral("t"));
    before.selection.selectFrame(42, 40, QStringLiteral("t"));
    before.selection.selectBlock(7, 1, QStringLiteral("t"));
    before.selection.selectBytes(1000, 24, QStringLiteral("t"));
    before.selection.selectProbe(16, 32, QStringLiteral("t"));
    before.selection.selectRect(QRect(8, 8, 64, 32), QStringLiteral("t"));
    before.picture.setPicture(1920, 1080);
    before.picture.zoomTo(2.0);
    before.picture.panBy(100, 50);
    before.picture.set_plane_mode(1);
    before.picture.set_stage(2);
    REQUIRE(before.dock.applyPreset(QStringLiteral("Compare")));
    before.state.setPanelState(
        {{QStringLiteral("focusedPanel"), QStringLiteral("syntax")}, {QStringLiteral("singleIndex"), 2}});
    saved = before.state.capture();
    REQUIRE_FALSE(saved.isEmpty());
    CHECK(saved.toUtf8().size() <= AndroidState::kMaxBytes);
    // §0.7.5: the same state gives the same bytes.
    CHECK(before.state.capture() == saved);

    Models after(dir.filePath(QStringLiteral("cfg2")));
    const QSignalSpy restored(&after.state, &AndroidState::restored);
    REQUIRE(after.state.restore(saved));
    CHECK(restored.count() == 1);

    REQUIRE(after.project.fileCount() == 1);
    const QString now = after.project.files().front().id;
    CHECK(after.project.files().front().path == QStringLiteral("content://provider/doc/b.mp4"));
    CHECK(after.project.files().front().track_index == 1);
    CHECK(after.project.activeFile() == now);

    CHECK(after.selection.file_id() == now);  // follows the file into its new id
    CHECK(after.selection.track_index() == 1);
    CHECK(after.selection.frame_decode_index() == 42);
    CHECK(after.selection.frame_display_index() == 40);
    CHECK(after.selection.block_id() == 7);
    CHECK(after.selection.level() == 1);
    CHECK(after.selection.byte_offset() == 1000);
    CHECK(after.selection.byte_length() == 24);
    CHECK(after.selection.probe() == QPoint(16, 32));
    CHECK(after.selection.selection_rect() == QRect(8, 8, 64, 32));
    CHECK(after.selection.origin() == QString::fromLatin1(AndroidState::kOrigin));

    // ViewState: plane and stage at once; zoom and pan once the picture is there again.
    CHECK(after.picture.plane_mode() == 1);
    CHECK(after.picture.stage() == 2);
    CHECK(after.picture.fit());
    after.picture.setPicture(1920, 1080);
    CHECK_FALSE(after.picture.fit());
    CHECK_THAT(after.picture.zoom(), WithinAbs(before.picture.zoom(), 1e-9));
    CHECK_THAT(after.picture.pan_x(), WithinAbs(before.picture.pan_x(), 1e-9));
    CHECK_THAT(after.picture.pan_y(), WithinAbs(before.picture.pan_y(), 1e-9));

    CHECK(after.dock.layoutName() == QStringLiteral("Compare"));
    CHECK(after.dock.toJson() == before.dock.toJson());
    CHECK(after.state.panelState().value(QStringLiteral("focusedPanel")).toString() == QStringLiteral("syntax"));
    CHECK(after.state.panelState().value(QStringLiteral("singleIndex")).toInt() == 2);

    // Captured again, the restored models give the same document, apart from the renumbered file id.
    QString again = after.state.capture();
    again.replace(QStringLiteral("\"") + now + QStringLiteral("\""), QStringLiteral("\"") + id + QStringLiteral("\""));
    CHECK(again == saved);
  }
}

TEST_CASE("a project that still has its files is kept and the selection maps onto it", "[fast][gui][android]") {
  ovt::app::testing::qt_app();
  const QTemporaryDir dir;
  Models before(dir.filePath(QStringLiteral("cfg1")));
  const QString id = before.project.addFile(dir.filePath(QStringLiteral("x.ivf")), FileRole::Single);
  before.selection.setSource(id, 0, QStringLiteral("t"));
  before.selection.selectFrame(3, 3, QStringLiteral("t"));
  const QString saved = before.state.capture();

  Models after(dir.filePath(QStringLiteral("cfg2")));
  static_cast<void>(after.project.addFile(dir.filePath(QStringLiteral("other.ivf")), FileRole::Single));
  const QString same = after.project.addFile(dir.filePath(QStringLiteral("x.ivf")), FileRole::Reference);
  REQUIRE(after.state.restore(saved));
  CHECK(after.project.fileCount() == 2);  // nothing added
  CHECK(after.selection.file_id() == same);
  CHECK(after.selection.frame_decode_index() == 3);
}

TEST_CASE("a view saved in fit mode refits; one restored with a picture applies at once", "[fast][gui][android]") {
  ovt::app::testing::qt_app();
  const QTemporaryDir dir;
  Models m(dir.filePath(QStringLiteral("cfg")));
  m.picture.setPicture(1920, 1080);
  const PictureController::ViewSnapshot fitted = m.picture.viewSnapshot();
  m.picture.zoomTo(4.0);
  REQUIRE_FALSE(m.picture.fit());
  m.picture.restoreView(fitted);
  CHECK(m.picture.fit());
  CHECK_THAT(m.picture.zoom(), WithinAbs(fitted.zoom, 1e-9));

  PictureController::ViewSnapshot zoomed = fitted;
  zoomed.fit = false;
  zoomed.zoom = 2.0;
  zoomed.pan_x = 500;
  zoomed.pan_y = 400;
  m.picture.restoreView(zoomed);
  CHECK_FALSE(m.picture.fit());
  CHECK_THAT(m.picture.zoom(), WithinAbs(2.0, 1e-9));
  CHECK_THAT(m.picture.pan_x(), WithinAbs(500, 1e-9));
  CHECK_THAT(m.picture.pan_y(), WithinAbs(400, 1e-9));
}

TEST_CASE("the saved state stays within 100 KB by dropping panel state, then layout", "[fast][gui][android]") {
  const QJsonObject state{
      {QStringLiteral("version"), 1},
      {QStringLiteral("selection"), QJsonObject{{QStringLiteral("file_id"), QStringLiteral("f1")}}},
      {QStringLiteral("layout"), QJsonObject{{QStringLiteral("name"), QString(600, QLatin1Char('l'))}}},
      {QStringLiteral("panel_state"), QJsonObject{{QStringLiteral("big"), QString(2000, QLatin1Char('p'))}}}};
  QStringList dropped;
  auto bytes = AndroidState::encode(state, 4000, &dropped);
  REQUIRE(bytes);
  CHECK(dropped.isEmpty());

  bytes = AndroidState::encode(state, 1500, &dropped);
  REQUIRE(bytes);
  CHECK(dropped == QStringList{QStringLiteral("panel_state")});
  const QJsonObject reduced = QJsonDocument::fromJson(bytes.value_or(QByteArray())).object();
  CHECK_FALSE(reduced.contains(QStringLiteral("panel_state")));
  CHECK(reduced.contains(QStringLiteral("layout")));
  CHECK(reduced.value(QStringLiteral("dropped")).toArray().size() == 1);

  bytes = AndroidState::encode(state, 300, &dropped);
  REQUIRE(bytes);
  CHECK(dropped == QStringList{QStringLiteral("panel_state"), QStringLiteral("layout")});
  CHECK(bytes.value_or(QByteArray(301, 0)).size() <= 300);

  CHECK_FALSE(AndroidState::encode(state, 10, &dropped));
}

TEST_CASE("a layout given by its preset name alone applies that preset", "[fast][gui][android]") {
  ovt::app::testing::qt_app();
  const QTemporaryDir dir;
  Models m(dir.filePath(QStringLiteral("cfg")));
  REQUIRE(m.dock.layoutName() != QStringLiteral("Compare"));
  REQUIRE(m.state.restore(QStringLiteral(R"({"version":1,"layout":{"preset":"Compare"}})")));
  CHECK(m.dock.layoutName() == QStringLiteral("Compare"));
}

TEST_CASE("a document of another version or not JSON restores nothing", "[fast][gui][android]") {
  ovt::app::testing::qt_app();
  const QTemporaryDir dir;
  Models m(dir.filePath(QStringLiteral("cfg")));
  m.selection.selectFrame(5, 5, QStringLiteral("t"));
  CHECK_FALSE(m.state.restore(QStringLiteral("{\"version\":2,\"selection\":{\"frame\":[9,9]}}")));
  CHECK_FALSE(m.state.restore(QStringLiteral("not json")));
  CHECK_FALSE(m.state.restore(QString()));
  CHECK(m.selection.frame_decode_index() == 5);
}

TEST_CASE("selection JSON keeps 'no block' and large byte offsets", "[fast][gui][android]") {
  SelectionModel::Snapshot s;
  s.byte_range = {5'000'000'000ULL, 12};
  s.bit_range = {77, 3};
  const SelectionModel::Snapshot back = AndroidState::selectionFromJson(AndroidState::selectionToJson(s));
  CHECK(back.block_id == SelectionModel::kNoBlock);
  CHECK(back.byte_range == s.byte_range);
  CHECK(back.bit_range == s.bit_range);
  CHECK(back.frame == s.frame);
  CHECK(back.probe == QPoint(-1, -1));
  CHECK(back.selection_rect == QRect());
}
