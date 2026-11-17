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

#include <functional>

class QTimer;

namespace ovt::app {

class Platform : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(bool isAndroid READ isAndroid CONSTANT)
  Q_PROPERTY(bool isMacOS READ isMacOS CONSTANT)
  Q_PROPERTY(bool isLinux READ isLinux CONSTANT)
  // Touch devices have no hover (§11.4.4); on Android a mouse or stylus (InputDevice sources) turns it on (§11.5.3).
  Q_PROPERTY(bool hasHover READ hasHover NOTIFY hasHoverChanged)
  // Width of the window in dp (Configuration.screenWidthDp on Android, §11.5.2): the input of the breakpoint table.
  // Writable so that tests can stand in for the platform.
  Q_PROPERTY(double widthDp READ widthDp WRITE setWidthDp NOTIFY widthDpChanged)
  // "fullscreen", "multiWindow" (split-screen, desktop windowing) or "desktop": how the window is hosted.
  Q_PROPERTY(QString windowingMode READ windowingMode WRITE setWindowingMode NOTIFY windowingModeChanged)
  // True when this process started without a settings.json: the first run of a fresh profile (§11.8).
  Q_PROPERTY(bool isFirstRun READ isFirstRun CONSTANT)
  Q_PROPERTY(QString configDir READ configDir CONSTANT)
  Q_PROPERTY(QString settingsPath READ settingsPath CONSTANT)
  Q_PROPERTY(QString layoutsPath READ layoutsPath CONSTANT)
  Q_PROPERTY(QString keymapPath READ keymapPath CONSTANT)
  Q_PROPERTY(QString rhiBackend READ rhiBackend CONSTANT)
  Q_PROPERTY(bool pictureHostAvailable READ pictureHostAvailable CONSTANT)

 public:
  Platform();
  ~Platform() override;

  static bool isAndroid();
  static bool isMacOS();
  static bool isLinux();
  [[nodiscard]] bool hasHover() const { return has_hover_; }
  [[nodiscard]] double widthDp() const { return width_dp_; }
  [[nodiscard]] QString windowingMode() const { return windowing_mode_; }
  void setHasHover(bool value);
  void setWidthDp(double value);
  void setWindowingMode(const QString& value);

  // The layout class of §11.5.2 for a width: "three" (>= 1200 dp), "two" (840-1199), "vertical" (600-839) and
  // "single" (< 600). Layouts switch on these names (CompactLayout.qml).
  Q_INVOKABLE static QString layoutClassForWidth(double width_dp);

  // Android: re-reads Configuration.screenWidthDp, the input devices and the multi-window state (platform_android.cpp;
  // `window_width_dp` is ignored there, see refresh()); elsewhere takes `window_width_dp` when it is positive. QML
  // calls it when the window resizes.
  // Calls `read` every `interval_ms` (Android polls its Configuration this way; tests pass a stand-in).
  void startPolling(std::function<void()> read, int interval_ms);

  Q_INVOKABLE void refresh(double window_width_dp = 0);

 Q_SIGNALS:
  void hasHoverChanged();
  void widthDpChanged();
  void windowingModeChanged();

 public:
  // Decided on the first call (main.cpp makes it before anything can write settings.json) and constant after.
  static bool isFirstRun();

  // OVT_CONFIG_DIR if set and non-empty, else the per-platform directory of §11.2.5 (".../ovt").
  static QString configDir();
  static QString settingsPath();
  static QString layoutsPath();
  static QString keymapPath();

  // RHI backend name of the process: "metal", "vulkan", "opengl", "d3d11", "software" or "null".
  // Valid after selectGraphicsApi(), otherwise it is the default of this platform.
  static QString rhiBackend();

  // True when this build has the PictureHost item (WI-1.405): Qt >= 6.7 (QQuickRhiItem) and libovt-render, desktop
  // only (Android's EMBEDDED host is WI-A.403). The Picture panel loads PictureView.qml only then.
  static bool pictureHostAvailable();

  // The backend fixed per platform (macOS Metal, Linux Vulkan, Android OpenGL ES), and the OVT_RHI override.
  // Returns false when `name` is not a backend name.
  static QSGRendererInterface::GraphicsApi defaultGraphicsApi();
  static bool parseGraphicsApi(const QString& name, QSGRendererInterface::GraphicsApi* api);
  static QString graphicsApiName(QSGRendererInterface::GraphicsApi api);

  // Chooses the RHI backend and calls QQuickWindow::setGraphicsApi. Call after the QGuiApplication exists and
  // before the first window. Order of precedence: QSG_RHI_BACKEND / QT_QUICK_BACKEND (Qt's own, left alone),
  // OVT_RHI, the software scene graph on the offscreen platform, the per-platform default.
  static void selectGraphicsApi();

 private:
  // Android: Configuration.screenWidthDp of the Activity, the input devices and the multi-window state
  // (platform_android.cpp). Elsewhere a no-op that returns false.
  bool refreshFromAndroid(bool read_width);

  bool has_hover_ = false;
  double width_dp_ = 1280;  // desktop default: the three-pane layout
  QString windowing_mode_;
  QTimer* poll_ = nullptr;
};

}  // namespace ovt::app

#endif  // OVT_APP_PLATFORM_PLATFORM_H
