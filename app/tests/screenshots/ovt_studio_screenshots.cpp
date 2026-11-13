// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The screenshot harness of chapter 14 §14.10.2 (WI-0.568): renders one named scene of OVT Studio offscreen at a
// fixed size and theme and writes `<out>/<scene>_<theme>.png`. `tools/imgdiff.py --mode ui` compares the result
// with the reference image under app/tests/screenshots/<platform>/ (see README.md in this directory).
//
//   ovt_studio_screenshots --scene <name> --theme light|dark --out <dir>
//   ovt_studio_screenshots --list-scenes
//
// Exit status: 0 = the PNG was written, 1 = the scene failed to render, 2 = bad arguments.

#include <QtQml/qqml.h>
#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

#include <array>
#include <cstdio>
#include <string_view>

#include "platform/platform.h"
#include "settings.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

// §14.10.2: fixed window size in device-independent pixels at 96 DPI and scale factor 1.
constexpr int kWindowWidth = 1600;
constexpr int kWindowHeight = 1000;
constexpr int kRenderTimeoutMs = 20000;
constexpr int kSettleFrames = 3;  // frames to wait for after the scene is shown, so animations and layouts settle

// The bundled UI font. System fonts are never used in tests, so a missing file is reported on stderr.
constexpr std::string_view kBundledFont = "app/resources/fonts/Inter-Variable.ttf";

// A scene is a QML component of the OvtStudio module (qrc URL) shown in a window of the fixed size. Scenes that need
// fixture files or scripted state grow here, with the work items that own them (WI-1.576 lists the phase-1 scenes of
// §14.10.2: container_tree_selected, syntax_tree_hex_selected, timeline_1000_frames, ...).
struct Scene {
  std::string_view name;
  const char* url;
};
// `welcome` (WI-1.464): a fresh profile (the harness uses an empty config directory), so the window shows the Welcome
// page and the privacy banner.
constexpr std::array<Scene, 2> kScenes{{
    {"main_window", "qrc:/qt/qml/OvtStudio/Main.qml"},
    {"welcome", "qrc:/qt/qml/OvtStudio/Main.qml"},
}};

const Scene* find_scene(const QString& name) {
  for (const Scene& scene : kScenes) {
    if (QString::fromUtf8(scene.name.data(), static_cast<qsizetype>(scene.name.size())) == name) {
      return &scene;
    }
  }
  return nullptr;
}

void report(const QString& message) {
  const QByteArray line = (QStringLiteral("ovt_studio_screenshots: ") + message + QLatin1Char('\n')).toLocal8Bit();
  static_cast<void>(std::fputs(line.constData(), stderr));
}

// The font of §14.10.2 from the source tree (OVT_SOURCE_DIR is set by CMake) or from OVT_TEST_FONT.
void load_bundled_font() {
  QString path = qEnvironmentVariable("OVT_TEST_FONT");
  if (path.isEmpty()) {
    path = QDir(QStringLiteral(OVT_SOURCE_DIR)).filePath(QString::fromUtf8(kBundledFont.data(), kBundledFont.size()));
  }
  if (!QFile::exists(path) || QFontDatabase::addApplicationFont(path) < 0) {
    report(QStringLiteral("warning: bundled font %1 not available; screenshots use the Qt default font and are not "
                          "comparable across machines (SPEC-ASSUMPTION(#353): the font file is added by WI-1.576)")
               .arg(path));
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  // §14.10.2: 96 DPI, scale factor 1. Set before the application object exists; the offscreen platform is the
  // default so the harness needs no display, but a caller may choose another (macOS nightly).
  qputenv("QT_SCALE_FACTOR", "1");
  qputenv("QT_FONT_DPI", "96");
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  // The scene must not read or write the user's settings.json.
  const QTemporaryDir config_dir;
  if (config_dir.isValid() && !qEnvironmentVariableIsSet("OVT_CONFIG_DIR")) {
    qputenv("OVT_CONFIG_DIR", config_dir.path().toLocal8Bit());
  }
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  const QGuiApplication app(argc, argv);

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("Renders one OVT Studio scene to a PNG (chapter 14 §14.10.2)."));
  parser.addHelpOption();
  const QCommandLineOption scene_option(QStringLiteral("scene"), QStringLiteral("Scene name."), QStringLiteral("name"));
  const QCommandLineOption theme_option(QStringLiteral("theme"), QStringLiteral("light or dark."),
                                        QStringLiteral("theme"));
  const QCommandLineOption out_option(QStringLiteral("out"), QStringLiteral("Output directory."),
                                      QStringLiteral("dir"));
  const QCommandLineOption list_option(QStringLiteral("list-scenes"), QStringLiteral("Print the scene names."));
  parser.addOptions({scene_option, theme_option, out_option, list_option});
  parser.process(app);

  if (parser.isSet(list_option)) {
    for (const Scene& scene : kScenes) {
      static_cast<void>(std::printf("%.*s\n", static_cast<int>(scene.name.size()), scene.name.data()));
    }
    return kExitOk;
  }
  const QString theme = parser.value(theme_option);
  if (!parser.isSet(scene_option) || !parser.isSet(out_option) ||
      (theme != QStringLiteral("light") && theme != QStringLiteral("dark"))) {
    report(QStringLiteral("usage: --scene <name> --theme light|dark --out <dir>"));
    return kExitUsage;
  }
  const Scene* scene = find_scene(parser.value(scene_option));
  if (scene == nullptr) {
    report(QStringLiteral("unknown scene \"%1\" (see --list-scenes)").arg(parser.value(scene_option)));
    return kExitUsage;
  }
  const QDir out_dir(parser.value(out_option));
  if (!out_dir.mkpath(QStringLiteral("."))) {
    report(QStringLiteral("cannot create %1").arg(out_dir.path()));
    return kExitFailure;
  }

  ovt::app::Platform::selectGraphicsApi();
  QQuickStyle::setStyle(QStringLiteral("Fusion"));
  load_bundled_font();

  QQmlApplicationEngine engine;
  engine.setInitialProperties({{QStringLiteral("title"), QStringLiteral("OVT Studio")}});
  engine.load(QUrl(QString::fromLatin1(scene->url)));
  auto* window =
      engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  if (window == nullptr) {
    report(QStringLiteral("scene %1 did not load or is not a window").arg(parser.value(scene_option)));
    return kExitFailure;
  }
  // The module is registered once the scene has imported it, so the singleton is looked up after loading.
  auto* settings = engine.singletonInstance<ovt::app::Settings*>(qmlTypeId("OvtStudio", 1, 0, "Settings"));
  if (settings == nullptr) {
    report(QStringLiteral("the Settings singleton is not available"));
    return kExitFailure;
  }
  settings->setTheme(theme);
  window->setMinimumSize({0, 0});
  window->resize(kWindowWidth, kWindowHeight);

  // Wait for a few swapped frames (the scene graph is then up to date), bounded by a timeout.
  int frames = 0;
  QEventLoop loop;
  QObject::connect(window, &QQuickWindow::frameSwapped, &loop, [&] {
    if (++frames >= kSettleFrames) {
      loop.quit();
    }
  });
  QObject::connect(window, &QQuickWindow::sceneGraphError, &loop,
                   [&](QQuickWindow::SceneGraphError, const QString& message) {
                     report(QStringLiteral("scene graph error: %1").arg(message));
                     loop.exit(kExitFailure);
                   });
  QTimer::singleShot(kRenderTimeoutMs, &loop, [&] {
    report(QStringLiteral("no frame rendered within %1 ms").arg(kRenderTimeoutMs));
    loop.exit(kExitFailure);
  });
  window->show();
  if (loop.exec() != kExitOk) {
    return kExitFailure;
  }

  QImage image = window->grabWindow();
  if (image.isNull()) {
    report(QStringLiteral("grabWindow() returned no image"));
    return kExitFailure;
  }
  // PNG with an opaque RGB format: no alpha channel and no colour-space chunk, so the bytes are reproducible.
  image = image.convertToFormat(QImage::Format_RGB888);
  const QString path = out_dir.filePath(parser.value(scene_option) + QLatin1Char('_') + theme + QStringLiteral(".png"));
  if (!image.save(path, "PNG")) {
    report(QStringLiteral("cannot write %1").arg(path));
    return kExitFailure;
  }
  static_cast<void>(std::printf("%s %dx%d\n", path.toLocal8Bit().constData(), image.width(), image.height()));
  return kExitOk;
}
