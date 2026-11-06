// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Qt Quick Test runner of app/tests/qml (WI-1.450). The tests use the same QML module as ovt-studio, with the
// settings directory redirected to a temporary one so they never touch the user's settings.json.

#include <QtQuickTest/quicktest.h>
#include <QObject>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtGlobal>

#include "platform/platform.h"

class Setup : public QObject {
  Q_OBJECT

 public Q_SLOTS:
  void applicationAvailable() {
    if (config_dir_.isValid()) {
      qputenv("OVT_CONFIG_DIR", config_dir_.path().toLocal8Bit());
    }
    ovt::app::Platform::selectGraphicsApi();
    // As in main.cpp: the default style on macOS is the native one, whose menus have no content item.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
  }

 private:
  QTemporaryDir config_dir_;
};

QUICK_TEST_MAIN_WITH_SETUP(tst_main, Setup)

#include "tst_main.moc"
