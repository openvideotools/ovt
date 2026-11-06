// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `Platform` QML singleton (chapter 11 §11.2.1, §11.2.5, §11.5.1; WI-1.450): which platform the GUI runs on,
// where its configuration lives and which Qt RHI backend was chosen. Platform differences in QML are expressed
// through this object and nowhere else.

#ifndef OVT_APP_PLATFORM_PLATFORM_H
#define OVT_APP_PLATFORM_PLATFORM_H

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QSGRendererInterface>
#include <QString>

namespace ovt::app {

class Platform : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(bool isAndroid READ isAndroid CONSTANT)
  Q_PROPERTY(bool isMacOS READ isMacOS CONSTANT)
  Q_PROPERTY(bool isLinux READ isLinux CONSTANT)
  Q_PROPERTY(bool hasHover READ hasHover CONSTANT)
  Q_PROPERTY(QString configDir READ configDir CONSTANT)
  Q_PROPERTY(QString settingsPath READ settingsPath CONSTANT)
  Q_PROPERTY(QString layoutsPath READ layoutsPath CONSTANT)
  Q_PROPERTY(QString keymapPath READ keymapPath CONSTANT)
  Q_PROPERTY(QString rhiBackend READ rhiBackend CONSTANT)

 public:
  Platform() = default;

  static bool isAndroid();
  static bool isMacOS();
  static bool isLinux();
  // Touch devices have no hover (§11.4.4).
  static bool hasHover();

  // OVT_CONFIG_DIR if set and non-empty, else the per-platform directory of §11.2.5 (".../ovt").
  static QString configDir();
  static QString settingsPath();
  static QString layoutsPath();
  static QString keymapPath();

  // RHI backend name of the process: "metal", "vulkan", "opengl", "d3d11", "software" or "null".
  // Valid after selectGraphicsApi(), otherwise it is the default of this platform.
  static QString rhiBackend();

  // The backend fixed per platform (macOS Metal, Linux Vulkan, Android OpenGL ES), and the OVT_RHI override.
  // Returns false when `name` is not a backend name.
  static QSGRendererInterface::GraphicsApi defaultGraphicsApi();
  static bool parseGraphicsApi(const QString& name, QSGRendererInterface::GraphicsApi* api);
  static QString graphicsApiName(QSGRendererInterface::GraphicsApi api);

  // Chooses the RHI backend and calls QQuickWindow::setGraphicsApi. Call after the QGuiApplication exists and
  // before the first window. Order of precedence: QSG_RHI_BACKEND / QT_QUICK_BACKEND (Qt's own, left alone),
  // OVT_RHI, the software scene graph on the offscreen platform, the per-platform default.
  static void selectGraphicsApi();
};

}  // namespace ovt::app

#endif  // OVT_APP_PLATFORM_PLATFORM_H
