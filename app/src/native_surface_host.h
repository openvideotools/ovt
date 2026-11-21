// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NativeSurfaceHost (chapter 11 §11.2.1, §11.10 WI-1.463; chapter 10 §10.1.2, §10.4.1): the NATIVE-mode host of the
// Picture panel on desktop. While HDR output is requested on an HDR-capable display it creates a child QWindow of the
// panel's window (a MetalSurface window on macOS), keeps it on top of the PictureHost item, and describes it as the
// NATIVE DisplayTarget that PictureHost's renderer hands to VideoSurface::retarget(). Otherwise the child window is
// hidden and the target is EMBEDDED (the PictureHost's offscreen texture).
//
//   1. Lifecycle: the child window is created the first time NATIVE mode is entered with the parent window exposed
//      (winId() forces the native window, §10.11 WI-1.406 pitfalls), then only shown and hidden; it dies with the host.
//      The platform part (the window's surface type, the native handle) is a NativeWindowBackend: macOS in
//      platform/mac_native_host.mm; other desktops have none in phase 1 (the Linux host is WI-4.450), so `available`
//      is false there and the host stays EMBEDDED.
//   2. Geometry: the item's rect mapped to the scene (Window.contentItem coordinates, device-independent pixels) is
//      the child window's geometry; the drawable is that size times the window's devicePixelRatio. It is re-mapped on
//      every x/y/width/height change of the item and of each ancestor, and on a parent or window change.
//   3. Popup rule (§11.5.5 item 3 on desktop; SPEC-ASSUMPTION(#702): only popups over the picture count, and an
//      unfocused window keeps NATIVE mode): while any popup, menu or dialog is open (a visible child of
//      Overlay.overlay) or the item is not visible (tab not current, area hidden), or the panel says so through
//      setVisible(false), the child window is hidden and the target goes back to EMBEDDED. Popups are not given
//      Popup.Window: one rule for every Qt version and for every popup of the shell.
//   4. SPEC-ASSUMPTION(#702): the HDR monitor window itself is a follow-up. The host is per Picture panel and maps
//   through the panel's own window, so a Picture panel in a secondary
//      window (the HDR monitor window of §11.2.1) gets its own child window on the display it is on.
//
// The host lives on the GUI thread. The renderer reads target() in QQuickRhiItemRenderer::synchronize() (GUI thread
// blocked) and calls VideoSurface::retarget() on the render thread when `generation` changed.

#ifndef OVT_APP_NATIVE_SURFACE_HOST_H
#define OVT_APP_NATIVE_SURFACE_HOST_H

#include <QtQml/qqmlregistration.h>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QRect>
#include <QString>
#include <QSurface>
#include <QTimer>
#include <QWindow>

#include <cstdint>
#include <memory>

namespace ovt::app {

// The window-system part of a NATIVE target (step 1). Implementations: platform/mac_native_host.mm; tests.
class NativeWindowBackend {
 public:
  NativeWindowBackend() = default;
  NativeWindowBackend(const NativeWindowBackend&) = delete;
  NativeWindowBackend& operator=(const NativeWindowBackend&) = delete;
  NativeWindowBackend(NativeWindowBackend&&) = delete;
  NativeWindowBackend& operator=(NativeWindowBackend&&) = delete;
  virtual ~NativeWindowBackend() = default;

  // The surface type of the child window (macOS: QSurface::MetalSurface).
  [[nodiscard]] virtual QSurface::SurfaceType surface_type() const = 0;
  // The render::DisplayTarget::Kind of the target (chapter 10 §10.2.2; macOS: Kind::MetalLayer = 1).
  [[nodiscard]] virtual int target_kind() const = 0;
  // DisplayTarget::native for `window`, whose native window exists (macOS: the CAMetalLayer* Qt made for the view);
  // nullptr when it cannot be had (the host then stays EMBEDDED and reports `error`).
  [[nodiscard]] virtual void* native_handle(QWindow& window, QString& error) = 0;
};

// This platform's backend (macOS: mac_native_host.mm), or nullptr where phase 1 has none.
std::unique_ptr<NativeWindowBackend> make_platform_native_backend();

// What the renderer needs to retarget (chapter 10 §10.2.2 DisplayTarget fields), without the render headers.
struct NativeTargetDesc {
  bool native = false;     // false: EMBEDDED (OffscreenTexture of the item's size)
  int kind = 0;            // render::DisplayTarget::Kind when native
  void* handle = nullptr;  // DisplayTarget::native
  int width = 0;           // drawable size in device pixels
  int height = 0;
  double device_pixel_ratio = 1.0;
  bool hdr_capable = false;      // DisplayCaps.hdr_capable (display_probe(), §10.4.5)
  double headroom = 1.0;         // DisplayCaps.headroom
  int encoding = 0;              // HdrView.macEncoding: 0 ScRgbLinear16F (default), 1 Pq10 ("System PQ pass-through")
  std::uint64_t generation = 0;  // bumped on every change; the renderer retargets when it differs from its own
};

class NativeSurfaceHost : public QObject {
  Q_OBJECT
  QML_ELEMENT
  // The item the child window covers (the PictureHost).
  Q_PROPERTY(QQuickItem* item READ item WRITE set_item NOTIFY itemChanged)
  // Overlay.overlay of the item's window: a visible child = an open popup (step 3).
  Q_PROPERTY(QQuickItem* overlay READ overlay WRITE set_overlay NOTIFY overlayChanged)
  // HDR output requested (View ▸ HDR output, Ctrl+H, the HDR panel toggle: HdrView.hdrOutput).
  Q_PROPERTY(bool requested READ requested WRITE set_requested NOTIFY stateChanged)
  // The display description from DisplayCaps; NATIVE mode needs hdrCapable (R-ARCH-2).
  Q_PROPERTY(bool hdrCapable READ hdr_capable WRITE set_hdr_capable NOTIFY stateChanged)
  Q_PROPERTY(double headroom READ headroom WRITE set_headroom NOTIFY stateChanged)
  Q_PROPERTY(int encoding READ encoding WRITE set_encoding NOTIFY stateChanged)
  // A platform backend exists (macOS in phase 1).
  Q_PROPERTY(bool available READ available CONSTANT)
  // The target is NATIVE: the child window is shown over the item.
  Q_PROPERTY(bool native READ native NOTIFY stateChanged)
  // NATIVE is requested but held back by the popup rule.
  Q_PROPERTY(bool suppressed READ suppressed NOTIFY stateChanged)
  Q_PROPERTY(bool popupOpen READ popup_open NOTIFY stateChanged)
  // The child window's geometry in the parent window (device-independent pixels).
  Q_PROPERTY(QRect geometry READ geometry NOTIFY geometryChanged)
  // Why NATIVE mode is not possible right now; "" when it is or was not requested.
  Q_PROPERTY(QString error READ error NOTIFY stateChanged)

 public:
  // The platform's backend.
  explicit NativeSurfaceHost(QObject* parent = nullptr);
  // `backend` may be nullptr (no NATIVE target on this platform).
  NativeSurfaceHost(std::unique_ptr<NativeWindowBackend> backend, QObject* parent);
  ~NativeSurfaceHost() override;
  NativeSurfaceHost(const NativeSurfaceHost&) = delete;
  NativeSurfaceHost& operator=(const NativeSurfaceHost&) = delete;
  NativeSurfaceHost(NativeSurfaceHost&&) = delete;
  NativeSurfaceHost& operator=(NativeSurfaceHost&&) = delete;

  // Re-evaluates the mode from the inputs and switches EMBEDDED <-> NATIVE: shows or hides the child window and
  // publishes a new target() (the renderer then calls VideoSurface::retarget()).
  Q_INVOKABLE void retarget();
  // The child window's geometry in device-independent pixels of the parent window (normally from the item's mapped
  // rect; the panel or a test may set it directly). An empty rect hides the child window.
  Q_INVOKABLE void setGeometry(const QRect& rect);
  // The panel's own visibility input to the popup rule; false hides the child window.
  Q_INVOKABLE void setVisible(bool visible);
  // The renderer could not retarget to NATIVE (VideoSurface::last_error()): back to EMBEDDED until the request or the
  // display changes. GUI thread.
  Q_INVOKABLE void reportNativeFailure(const QString& error);

  [[nodiscard]] QQuickItem* item() const { return item_; }
  void set_item(QQuickItem* item);
  [[nodiscard]] QQuickItem* overlay() const { return overlay_; }
  void set_overlay(QQuickItem* overlay);
  [[nodiscard]] bool requested() const noexcept { return requested_; }
  void set_requested(bool on);
  [[nodiscard]] bool hdr_capable() const noexcept { return hdr_capable_; }
  void set_hdr_capable(bool on);
  [[nodiscard]] double headroom() const noexcept { return headroom_; }
  void set_headroom(double headroom);
  [[nodiscard]] int encoding() const noexcept { return encoding_; }
  void set_encoding(int encoding);
  [[nodiscard]] bool available() const noexcept { return backend_ != nullptr; }
  [[nodiscard]] bool native() const noexcept { return target_.native; }
  [[nodiscard]] bool suppressed() const;
  [[nodiscard]] bool popup_open() const noexcept { return popup_open_; }
  [[nodiscard]] QRect geometry() const { return geometry_; }
  [[nodiscard]] QString error() const { return error_; }

  // The current target; read by the renderer during synchronize().
  [[nodiscard]] const NativeTargetDesc& target() const noexcept { return target_; }
  // The child window (nullptr until NATIVE mode was first entered); for tests and the platform glue.
  [[nodiscard]] QWindow* childWindow() const { return child_.data(); }
  // The item's rect in the scene (Window.contentItem coordinates), rounded to whole device-independent pixels.
  [[nodiscard]] static QRect mappedRect(const QQuickItem& item);

 Q_SIGNALS:
  void itemChanged();
  void overlayChanged();
  void stateChanged();
  void geometryChanged();
  // target() changed (mode, size, display or encoding): the renderer should synchronize.
  void targetChanged();
  // While NATIVE: every 500 ms, so that the renderer polls the EDR headroom (§10.4.1).
  void refreshRequested();

 private:
  void track_item();
  void sync_geometry();
  void update_popup_state();
  [[nodiscard]] bool want_native() const;
  bool ensure_child(QString& why);
  void publish(bool native);

  std::unique_ptr<NativeWindowBackend> backend_;
  QPointer<QQuickItem> item_;
  QPointer<QQuickItem> overlay_;
  QPointer<QWindow> parent_window_;
  QPointer<QWindow> child_;  // owned: deleted in the destructor or when the item moves to another window
  void* handle_ = nullptr;
  QList<QMetaObject::Connection> item_connections_;
  QList<QMetaObject::Connection> overlay_connections_;
  QMetaObject::Connection exposed_connection_;
  bool waiting_for_expose_ = false;
  QTimer refresh_;
  bool requested_ = false;
  bool hdr_capable_ = false;
  double headroom_ = 1.0;
  int encoding_ = 0;
  bool panel_visible_ = true;
  bool popup_open_ = false;
  bool failed_ = false;  // reportNativeFailure() since the last change of request or display
  QRect geometry_;
  QString error_;
  NativeTargetDesc target_;
};

}  // namespace ovt::app

#endif  // OVT_APP_NATIVE_SURFACE_HOST_H
