// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.466 acceptance test 1: the §11.4.3 map, keymap.json overrides and GUI-KEY-0001.

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "keymap.h"
#include "qt_test_app.h"

namespace {

using ovt::app::actionRegistry;
using ovt::app::Keymap;

void write_file(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  REQUIRE(file.open(QIODevice::WriteOnly));
  file.write(bytes);
}

QString keymap_path(const QTemporaryDir& dir) {
  return dir.path() + QStringLiteral("/keymap.json");
}

}  // namespace

TEST_CASE("Keymap: the defaults are the §11.4.3 table and do not conflict", "[fast][gui][keymap]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const Keymap keymap(keymap_path(dir));
  CHECK(keymap.diagnostics().isEmpty());
  CHECK(keymap.keys(QStringLiteral("action.navigate.next")) == QStringList{QStringLiteral("Right")});
  CHECK(keymap.keys(QStringLiteral("action.edit.goTo")) == QStringList{QStringLiteral("Ctrl+G")});
  CHECK(keymap.keys(QStringLiteral("action.overlays.mode.f12")) == QStringList{QStringLiteral("F12")});
  CHECK(keymap.keys(QStringLiteral("action.view.plane.gray")) == QStringList{QStringLiteral("5")});
  // Every key is valid portable text, and no two actions of one scope share one.
  QSet<QString> seen;
  for (const auto& action : actionRegistry()) {
    CHECK_FALSE(action.keys.isEmpty());
    for (const QString& key : action.keys) {
      INFO(action.id.toStdString() << " " << key.toStdString());
      CHECK(Keymap::normalise(key) == key);
      const QString scoped = action.scope + QLatin1Char('|') + key;
      CHECK_FALSE(seen.contains(scoped));
      seen.insert(scoped);
    }
  }
  CHECK(keymap.windowActionIds().size() < keymap.actionIds().size());
}

TEST_CASE("Keymap: keymap.json rebinds an action and the dialog's data shows it", "[fast][gui][keymap]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  // L is the legend toggle by default, so the file moves the legend away from it, as a user would.
  write_file(keymap_path(dir),
             R"({ "action.navigate.next": ["Right", "L"], "action.overlays.legend": ["Ctrl+L"], "x.unknown": ["Q"] })");
  Keymap keymap(keymap_path(dir));
  CHECK(keymap.diagnostics().isEmpty());
  CHECK(keymap.keys(QStringLiteral("action.navigate.next")) ==
        QStringList{QStringLiteral("Right"), QStringLiteral("L")});
  CHECK(keymap.display(QStringLiteral("action.navigate.next")) == QStringLiteral("Right / L"));
  CHECK(keymap.matches(QStringLiteral("action.navigate.next"), Qt::Key_L, 0));
  CHECK(keymap.matches(QStringLiteral("action.overlays.legend"), Qt::Key_L, Qt::ControlModifier));
  CHECK_FALSE(keymap.matches(QStringLiteral("action.overlays.legend"), Qt::Key_L, 0));
  // Unknown ids survive a save.
  CHECK(keymap.setKeys(QStringLiteral("action.navigate.previous"), {QStringLiteral("J")}).isEmpty());
  QFile file(keymap_path(dir));
  REQUIRE(file.open(QIODevice::ReadOnly));
  const QJsonObject written = QJsonDocument::fromJson(file.readAll()).object();
  CHECK(written.contains(QStringLiteral("x.unknown")));
  CHECK(written.contains(QStringLiteral("action.navigate.previous")));
}

TEST_CASE("Keymap: a binding already used is rejected with GUI-KEY-0001", "[fast][gui][keymap]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  write_file(keymap_path(dir), R"({ "action.navigate.next": ["Right", "L"], "action.navigate.previous": ["Ctrl+Q"] })");
  const Keymap keymap(keymap_path(dir));
  // L belongs to the legend and Ctrl+Q to Quit: both overriding entries are rejected, the defaults stay.
  CHECK(keymap.diagnosticCode() == QStringLiteral("GUI-KEY-0001"));
  REQUIRE(keymap.diagnostics().size() == 2);
  CHECK(keymap.diagnostics().at(0).startsWith(QStringLiteral("GUI-KEY-0001: shortcut ")));
  CHECK(keymap.keys(QStringLiteral("action.navigate.next")) == QStringList{QStringLiteral("Right")});
  CHECK(keymap.keys(QStringLiteral("action.navigate.previous")) == QStringList{QStringLiteral("Left")});
  CHECK(keymap.keys(QStringLiteral("action.overlays.legend")) == QStringList{QStringLiteral("L")});
  CHECK(keymap.keys(QStringLiteral("action.file.quit")) == QStringList{QStringLiteral("Ctrl+Q")});
}

TEST_CASE("Keymap: setKeys refuses a conflict, accepts a free key, resets", "[fast][gui][keymap]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  Keymap keymap(keymap_path(dir));
  const QString message = keymap.setKeys(QStringLiteral("action.navigate.next"), {QStringLiteral("L")});
  CHECK(message.startsWith(QStringLiteral("GUI-KEY-0001: shortcut L is bound to action.overlays.legend")));
  CHECK(keymap.keys(QStringLiteral("action.navigate.next")) == QStringList{QStringLiteral("Right")});
  CHECK_FALSE(QFile::exists(keymap_path(dir)));  // nothing changed, nothing written
  CHECK(keymap.setKeys(QStringLiteral("action.navigate.next"), {QStringLiteral("ctrl+alt+k")}).isEmpty());
  CHECK(keymap.keys(QStringLiteral("action.navigate.next")) == QStringList{QStringLiteral("Ctrl+Alt+K")});
  CHECK(QFile::exists(keymap_path(dir)));
  const int revision = keymap.revision();
  keymap.resetAction(QStringLiteral("action.navigate.next"));
  CHECK(keymap.keys(QStringLiteral("action.navigate.next")) == QStringList{QStringLiteral("Right")});
  CHECK(keymap.revision() > revision);
  CHECK_FALSE(keymap.setKeys(QStringLiteral("action.navigate.next"), {QStringLiteral("NotAKey")}).isEmpty());
  // A panel-local key does not conflict with a window one: Shift+Right is both "forward 10" and "select right".
  CHECK(keymap.conflictOf(QStringLiteral("action.picture.selectRight"), QStringLiteral("Shift+Right")).isEmpty());
}

TEST_CASE("Keymap: a damaged file gives the default map and GUI-KEY-0001", "[fast][gui][keymap]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  write_file(keymap_path(dir), "{ not json");
  const Keymap keymap(keymap_path(dir));
  CHECK(keymap.diagnosticCode() == QStringLiteral("GUI-KEY-0001"));
  CHECK(keymap.keys(QStringLiteral("action.navigate.next")) == QStringList{QStringLiteral("Right")});
}

TEST_CASE("Keymap: key presses to portable text and back", "[fast][gui][keymap]") {
  const QTemporaryDir dir;
  const Keymap keymap(keymap_path(dir));
  CHECK(keymap.keyText(Qt::Key_O, Qt::ControlModifier) == QStringLiteral("Ctrl+O"));
  CHECK(keymap.keyText(Qt::Key_Right, Qt::ShiftModifier) == QStringLiteral("Shift+Right"));
  CHECK(keymap.keyText(Qt::Key_Shift, Qt::ShiftModifier).isEmpty());
  CHECK(keymap.decompose(QStringLiteral("Ctrl+Shift+O")).at(0).toInt() == Qt::Key_O);
}
