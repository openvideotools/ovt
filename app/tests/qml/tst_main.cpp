// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Qt Quick Test runner of app/tests/qml (WI-1.450). The tests use the same QML module as ovt-studio, with the
// settings directory redirected to a temporary one so they never touch the user's settings.json.

#include <QtQml/qqml.h>
#include <QtQuickTest/quicktest.h>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtGlobal>

#include "models/selection_model.h"
#include "models/stream_index_model.h"
#include "platform/platform.h"
#include "project.h"
#include "synthetic_index.h"

class Setup : public QObject {
  Q_OBJECT

 public Q_SLOTS:
  void qmlEngineAvailable(QQmlEngine* engine) {
    using namespace ovt::app;
    using namespace ovt::app::testing;
    engine->addImageProvider(QStringLiteral("rawpreview"), new RawPreviewProvider);
    // The Timeline tests (tst_timeline.qml) need an index: 300 frames, 60 fps in a 90 kHz time base, a GOP of 60.
    index_model_.setIndex(StreamIndexHandle::adopt(build_index(gop60(300, 1500), 1, 90000)));
    engine_ = engine;
    engine->rootContext()->setContextProperty(QStringLiteral("testIndex"), &index_model_);
    engine->rootContext()->setContextProperty(QStringLiteral("testSetup"), this);
  }

  // Called by tst_timeline.qml once the module is loaded: the Selection singleton derives unit and sample from the
  // same index.
  void attachSelection() {
    const int type_id = qmlTypeId("OvtStudio", 1, 0, "Selection");
    if (auto* selection = engine_->singletonInstance<ovt::app::SelectionModel*>(type_id)) {
      selection->setIndex(index_model_.selectionIndex());
    }
  }

  void applicationAvailable() {
    if (config_dir_.isValid()) {
      qputenv("OVT_CONFIG_DIR", config_dir_.path().toLocal8Bit());
    }
    // A fresh profile: decide first-run now, before any test writes settings.json (WI-1.464).
    static_cast<void>(ovt::app::Platform::isFirstRun());
    ovt::app::Platform::selectGraphicsApi();
    // As in main.cpp: the default style on macOS is the native one, whose menus have no content item.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
  }

 private:
  QTemporaryDir config_dir_;
  QQmlEngine* engine_ = nullptr;
  ovt::app::StreamIndexModel index_model_;
};

QUICK_TEST_MAIN_WITH_SETUP(tst_main, Setup)

#include "tst_main.moc"
