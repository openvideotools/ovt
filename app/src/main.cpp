// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry point of OVT Studio (WI-0.019): an empty Qt Quick window titled OVT_PRODUCT_NAME, and
// `--self-test`, which exits 0 once the first frame has been rendered (CI smoke, chapter 13 §13.7.1).
// WI-1.450 extends this file in place with the application shell (chapter 16 §16.3).

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtGlobal>

#include <cstdio>

#include "ovt/ovt.h"
#include "ovt_version_full.h"  // generated: OVT_VERSION_FULL = <VERSION>+<git sha>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;

// A frame has to appear well inside the 5 s budget of WI-0.019 acceptance test 1; the watchdog turns a
// hang (no frame, no event) into a failure with a message instead of a CTest timeout.
constexpr int kSelfTestWatchdogMs = 4000;

// The offscreen platform plugin (QT_QPA_PLATFORM=offscreen, used by CI and the self-test) has no GPU
// context on a headless machine, so the RHI backends cannot start there. Use the software scene graph
// unless the caller chose a backend explicitly.
void choose_scene_graph_backend() {
  if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
    return;
  }
  if (qEnvironmentVariableIsSet("QSG_RHI_BACKEND") || qEnvironmentVariableIsSet("QT_QUICK_BACKEND")) {
    return;
  }
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
}

// Diagnostics go to stderr with the product name; there is no logger in the GUI yet (WI-1.512 brings ovt::log).
void report(const QString& message) {
  const QByteArray line = (QStringLiteral(OVT_PRODUCT_NAME_VALUE ": ") + message + QLatin1Char('\n')).toLocal8Bit();
  static_cast<void>(std::fputs(line.constData(), stderr));  // best effort: nothing to do if stderr is closed
}

}  // namespace

// Exported: on Android this binary is libovt_studio_<ABI>.so and Qt finds main() with dlsym(), but
// ovt::flags compiles with -fvisibility=hidden (chapter 13 §13.2.5).
Q_DECL_EXPORT int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral(OVT_PRODUCT_NAME_VALUE));
  QGuiApplication::setApplicationDisplayName(QStringLiteral(OVT_PRODUCT_NAME_VALUE));
  QGuiApplication::setApplicationVersion(QStringLiteral(OVT_VERSION_FULL));
  QGuiApplication::setOrganizationDomain(QStringLiteral("ovt.dev"));
  QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/OvtStudio/resources/icons/ovt-studio.svg")));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(OVT_PRODUCT_NAME_VALUE));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption self_test(QStringLiteral("self-test"),
                                     QStringLiteral("Open the main window, exit 0 after the first rendered frame."));
  parser.addOption(self_test);
  parser.process(app);  // exits on --help, --version and unknown options
  const bool self_testing = parser.isSet(self_test);

  choose_scene_graph_backend();

  QQmlApplicationEngine engine;
  engine.setInitialProperties({{QStringLiteral("title"), QStringLiteral(OVT_PRODUCT_NAME_VALUE)}});
  engine.load(QUrl(QStringLiteral("qrc:/qt/qml/OvtStudio/Main.qml")));
  if (engine.rootObjects().isEmpty()) {
    report(QStringLiteral("could not load the main window"));
    return kExitFailure;
  }
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  if (window == nullptr) {
    report(QStringLiteral("the main QML object is not a window"));
    return kExitFailure;
  }

  QTimer watchdog;
  if (self_testing) {
    // Queued: leave the render loop before quitting. exit() is idempotent if a second frame arrives.
    QObject::connect(
        window, &QQuickWindow::frameSwapped, &app,
        [] {
          report(QStringLiteral("self-test passed (first frame, platform %1, core %2)")
                     .arg(QGuiApplication::platformName(), QString::fromUtf8(ovt_version_string())));
          QCoreApplication::exit(kExitOk);
        },
        Qt::QueuedConnection);
    QObject::connect(window, &QQuickWindow::sceneGraphError, &app,
                     [](QQuickWindow::SceneGraphError /*error*/, const QString& message) {
                       report(QStringLiteral("self-test failed: scene graph error: %1").arg(message));
                       QCoreApplication::exit(kExitFailure);
                     });
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &app, [] {
      report(QStringLiteral("self-test failed: no frame rendered within %1 ms").arg(kSelfTestWatchdogMs));
      QCoreApplication::exit(kExitFailure);
    });
    watchdog.start(kSelfTestWatchdogMs);
  }

  return QGuiApplication::exec();
}
