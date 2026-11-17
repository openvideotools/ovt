// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/platform.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

namespace ovt::app {

namespace {

QSGRendererInterface::GraphicsApi g_selected = QSGRendererInterface::Unknown;

}  // namespace

bool Platform::isAndroid() {
#ifdef Q_OS_ANDROID
  return true;
#else
  return false;
#endif
}

bool Platform::isMacOS() {
#ifdef Q_OS_MACOS
  return true;
#else
  return false;
#endif
}

bool Platform::isLinux() {
#ifdef Q_OS_LINUX
  return !isAndroid();
#else
  return false;
#endif
}

Platform::Platform()
    : has_hover_(!isAndroid()),
      windowing_mode_(isAndroid() ? QStringLiteral("fullscreen") : QStringLiteral("desktop")) {
  if (isAndroid()) {
    // The start-up width. Later changes are pushed by MainActivity.onConfigurationChanged through AndroidShell.qml
    // and refresh(), at the moment Android reports them (no polling), never from a QML item width, which is in
    // device pixels on Android.
    refreshFromAndroid(true);
  }
}

Platform::~Platform() = default;

void Platform::setHasHover(bool value) {
  if (has_hover_ != value) {
    has_hover_ = value;
    Q_EMIT hasHoverChanged();
  }
}

void Platform::setWidthDp(double value) {
  if (value > 0 && width_dp_ != value) {
    width_dp_ = value;
    Q_EMIT widthDpChanged();
  }
}

void Platform::setWindowingMode(const QString& value) {
  if (windowing_mode_ != value) {
    windowing_mode_ = value;
    Q_EMIT windowingModeChanged();
  }
}

QString Platform::layoutClassForWidth(double width_dp) {
  // §11.5.2: smallest-width breakpoints.
  if (width_dp >= 1200) {
    return QStringLiteral("three");
  }
  if (width_dp >= 840) {
    return QStringLiteral("two");
  }
  if (width_dp >= 600) {
    return QStringLiteral("vertical");
  }
  return QStringLiteral("single");
}

void Platform::refresh(double window_width_dp) {
  // On Android the width is Configuration.screenWidthDp (pushed by MainActivity, or read here when none is given),
  // never a Qt item width: the QtQuickView content is sized in device pixels there (device pixel ratio 1 on the
  // emulator).
  if (isAndroid()) {
    refreshFromAndroid(window_width_dp <= 0);
  }
  if (window_width_dp > 0) {
    setWidthDp(window_width_dp);
  }
}

#ifndef Q_OS_ANDROID
bool Platform::refreshFromAndroid(bool /*read_width*/) {
  return false;
}
#endif

bool Platform::isFirstRun() {
  static const bool first_run = !QFileInfo::exists(settingsPath());
  return first_run;
}

QString Platform::configDir() {
  const QByteArray override_dir = qgetenv("OVT_CONFIG_DIR");
  if (!override_dir.isEmpty()) {
    return QDir::cleanPath(QString::fromLocal8Bit(override_dir));
  }
  // §11.2.5 names the resulting directories: $XDG_CONFIG_HOME/ovt (Linux), ~/Library/Application Support/ovt
  // (macOS) and <filesDir>/ovt (Android). Qt's AppConfigLocation would append the application name instead of
  // "ovt" and is ~/Library/Preferences on macOS, so the generic locations are used.
  // SPEC-ASSUMPTION(#117): the listed paths win over the name AppConfigLocation.
#if defined(Q_OS_ANDROID)
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#elif defined(Q_OS_MACOS)
  const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#else
  const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#endif
  return QDir::cleanPath(base + QStringLiteral("/ovt"));
}

QString Platform::settingsPath() {
  return configDir() + QStringLiteral("/settings.json");
}
QString Platform::layoutsPath() {
  return configDir() + QStringLiteral("/layouts.json");
}
QString Platform::keymapPath() {
  return configDir() + QStringLiteral("/keymap.json");
}

QSGRendererInterface::GraphicsApi Platform::defaultGraphicsApi() {
#if defined(Q_OS_MACOS)
  return QSGRendererInterface::Metal;
#elif defined(Q_OS_ANDROID)
  return QSGRendererInterface::OpenGL;
#else
  return QSGRendererInterface::Vulkan;
#endif
}

bool Platform::parseGraphicsApi(const QString& name, QSGRendererInterface::GraphicsApi* api) {
  const QString key = name.trimmed().toLower();
  struct Entry {
    std::string_view name;
    QSGRendererInterface::GraphicsApi api;
  };
  static constexpr std::array<Entry, 7> kEntries{{
      {.name = "metal", .api = QSGRendererInterface::Metal},
      {.name = "vulkan", .api = QSGRendererInterface::Vulkan},
      {.name = "opengl", .api = QSGRendererInterface::OpenGL},
      {.name = "gles", .api = QSGRendererInterface::OpenGL},
      {.name = "d3d11", .api = QSGRendererInterface::Direct3D11},
      {.name = "software", .api = QSGRendererInterface::Software},
      {.name = "null", .api = QSGRendererInterface::NullRhi},
  }};
  const auto* const found =
      std::ranges::find_if(kEntries, [&key](const Entry& entry) { return key == QLatin1StringView(entry.name); });
  if (found == kEntries.end()) {
    return false;
  }
  if (api != nullptr) {
    *api = found->api;
  }
  return true;
}

QString Platform::graphicsApiName(QSGRendererInterface::GraphicsApi api) {
  switch (api) {
    case QSGRendererInterface::Metal:
      return QStringLiteral("metal");
    case QSGRendererInterface::Vulkan:
      return QStringLiteral("vulkan");
    case QSGRendererInterface::OpenGL:
      return QStringLiteral("opengl");
    case QSGRendererInterface::Direct3D11:
      return QStringLiteral("d3d11");
    case QSGRendererInterface::Software:
      return QStringLiteral("software");
    case QSGRendererInterface::NullRhi:
      return QStringLiteral("null");
    default:
      return QStringLiteral("unknown");
  }
}

bool Platform::pictureHostAvailable() {
#if defined(OVT_HAVE_PICTURE_HOST)
  return true;
#else
  return false;
#endif
}

QString Platform::rhiBackend() {
  return graphicsApiName(g_selected == QSGRendererInterface::Unknown ? defaultGraphicsApi() : g_selected);
}

void Platform::selectGraphicsApi() {
  if (qEnvironmentVariableIsSet("QSG_RHI_BACKEND") || qEnvironmentVariableIsSet("QT_QUICK_BACKEND")) {
    return;  // Qt's own override wins; rhiBackend() then reports the default of this platform
  }
  QSGRendererInterface::GraphicsApi api = defaultGraphicsApi();
  const QByteArray requested = qgetenv("OVT_RHI");
  if (!requested.isEmpty()) {
    if (!parseGraphicsApi(QString::fromLocal8Bit(requested), &api)) {
      static_cast<void>(std::fprintf(stderr,
                                     "OVT_RHI=%s is not a backend (metal, vulkan, opengl, d3d11, software, null)\n",
                                     requested.constData()));
    }
  } else if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
    // The offscreen platform plugin (CI, tests) has no GPU context on a headless machine.
    api = QSGRendererInterface::Software;
  }
  g_selected = api;
  QQuickWindow::setGraphicsApi(api);
}

}  // namespace ovt::app
