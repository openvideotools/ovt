// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/platform.h"

#include <QByteArray>
#include <QDir>
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

bool Platform::hasHover() {
  return !isAndroid();
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
