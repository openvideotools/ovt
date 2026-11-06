// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry point of OVT Studio (WI-0.019, WI-1.450): the main window of chapter 11 §11.2 (menus, toolbar, status bar),
// `--version`, and `--self-test`, which exits 0 once the first frame has been rendered (CI smoke, chapter 13
// §13.7.1). Start-up order: `--version` (needs no display) → high-DPI policy → QGuiApplication → RHI backend
// (Platform::selectGraphicsApi) → style → QML. Settings load when QML first uses the Settings singleton.

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtGlobal>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

#include "ovt/ovt.h"
#include "ovt_version_full.h"  // generated: OVT_VERSION_FULL = <VERSION>+<git sha>
#include "platform/platform.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;

// A frame has to appear well inside the 5 s budget of WI-0.019 acceptance test 1; the watchdog turns a
// hang (no frame, no event) into a failure with a message instead of a CTest timeout.
constexpr int kSelfTestWatchdogMs = 4000;

// Diagnostics go to stderr with the product name; there is no logger in the GUI yet (WI-1.512 brings ovt::log).
void report(const QString& message) {
  const QByteArray line = (QStringLiteral(OVT_PRODUCT_NAME_VALUE ": ") + message + QLatin1Char('\n')).toLocal8Bit();
  static_cast<void>(std::fputs(line.constData(), stderr));  // best effort: nothing to do if stderr is closed
}

// `--version` runs before the QGuiApplication exists, so it works without a display.
bool wants_version(std::span<char*> args) {
  return std::ranges::any_of(args.subspan(1), [](const char* arg) {
    return std::string_view(arg) == "--version" || std::string_view(arg) == "-v";
  });
}

int print_version() {
  static_cast<void>(std::printf(OVT_PRODUCT_NAME_VALUE " %s\ncore %s\nQt %s (built with " QT_VERSION_STR ")\n",
                                OVT_VERSION_FULL, ovt_version_string(), qVersion()));
  return kExitOk;
}

}  // namespace

// Exported: on Android this binary is libovt_studio_<ABI>.so and Qt finds main() with dlsym(), but
// ovt::flags compiles with -fvisibility=hidden (chapter 13 §13.2.5).
Q_DECL_EXPORT int main(int argc, char* argv[]) {
  if (wants_version(std::span<char*>(argv, static_cast<std::size_t>(argc)))) {
    return print_version();
  }
  // Before the application object (WI-1.450): the scale factor is passed through unrounded, so 125 % stays 125 %.
  // Qt::AA_EnableHighDpiScaling is a no-op in Qt 6 and is not used.
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral(OVT_PRODUCT_NAME_VALUE));
  QGuiApplication::setApplicationDisplayName(QStringLiteral(OVT_PRODUCT_NAME_VALUE));
  QGuiApplication::setApplicationVersion(QStringLiteral(OVT_VERSION_FULL));
  QGuiApplication::setOrganizationDomain(QStringLiteral("ovt.dev"));
  QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/OvtStudio/resources/icons/ovt-studio.svg")));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(OVT_PRODUCT_NAME_VALUE));
  parser.addHelpOption();
  const QCommandLineOption self_test(QStringLiteral("self-test"),
                                     QStringLiteral("Open the main window, exit 0 after the first rendered frame."));
  parser.addOption(self_test);
  parser.process(app);  // exits on --help and unknown options; --version was handled above
  const bool self_testing = parser.isSet(self_test);

  ovt::app::Platform::selectGraphicsApi();
  QQuickStyle::setStyle(QStringLiteral("Fusion"));  // OvtStyle.qml supplies the palette (§11.4.5)

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
