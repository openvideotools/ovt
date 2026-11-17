// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NativeSurfaceHost (WI-1.463); see native_surface_host.h for the design.

#include "native_surface_host.h"

#include <QQuickWindow>
#include <QRectF>
#include <QtGlobal>

#include <cmath>
#include <utility>

namespace ovt::app {

namespace {

// §10.4.1: the headroom is polled every 500 ms while NATIVE mode is active.
constexpr int kRefreshMs = 500;

bool same_target(const NativeTargetDesc& a, const NativeTargetDesc& b) {
  return a.native == b.native && a.kind == b.kind && a.handle == b.handle && a.width == b.width &&
         a.height == b.height && qFuzzyCompare(a.device_pixel_ratio, b.device_pixel_ratio) &&
         a.hdr_capable == b.hdr_capable && qFuzzyCompare(a.headroom, b.headroom) && a.encoding == b.encoding;
}

}  // namespace

#if !defined(__APPLE__)
// Phase 1 has the macOS host only (platform/mac_native_host.mm); the Linux host over a VulkanSurface child window is
// WI-4.450 (chapter 16 §16.3).
std::unique_ptr<NativeWindowBackend> make_platform_native_backend() {
  return nullptr;
}
#endif

NativeSurfaceHost::NativeSurfaceHost(QObject* parent) : NativeSurfaceHost(make_platform_native_backend(), parent) {}

NativeSurfaceHost::NativeSurfaceHost(std::unique_ptr<NativeWindowBackend> backend, QObject* parent)
    : QObject(parent), backend_(std::move(backend)) {
  refresh_.setInterval(kRefreshMs);
  connect(&refresh_, &QTimer::timeout, this, &NativeSurfaceHost::refreshRequested);
}

NativeSurfaceHost::~NativeSurfaceHost() {
  for (const QMetaObject::Connection& c : std::as_const(item_connections_)) {
    disconnect(c);
  }
  for (const QMetaObject::Connection& c : std::as_const(overlay_connections_)) {
    disconnect(c);
  }
  disconnect(exposed_connection_);
  // The renderer's NATIVE output keeps its own reference to the platform layer, so the window can go first.
  delete child_.data();
}

QRect NativeSurfaceHost::mappedRect(const QQuickItem& item) {
  const QRectF r = item.mapRectToScene(QRectF(0.0, 0.0, item.width(), item.height()));
  const auto left = static_cast<int>(std::lround(r.left()));
  const auto top = static_cast<int>(std::lround(r.top()));
  const auto right = static_cast<int>(std::lround(r.right()));
  const auto bottom = static_cast<int>(std::lround(r.bottom()));
  return {left, top, right - left, bottom - top};
}

// ---- Inputs -----------------------------------------------------------------------------------------------------

void NativeSurfaceHost::set_item(QQuickItem* item) {
  if (item == item_) {
    return;
  }
  item_ = item;
  track_item();
  Q_EMIT itemChanged();
}

void NativeSurfaceHost::set_overlay(QQuickItem* overlay) {
  if (overlay == overlay_) {
    return;
  }
  overlay_ = overlay;
  for (const QMetaObject::Connection& c : std::as_const(overlay_connections_)) {
    disconnect(c);
  }
  overlay_connections_.clear();
  if (overlay_ != nullptr) {
    overlay_connections_.append(
        connect(overlay_, &QQuickItem::childrenChanged, this, &NativeSurfaceHost::update_popup_state));
  }
  Q_EMIT overlayChanged();
  update_popup_state();
}

void NativeSurfaceHost::set_requested(bool on) {
  if (on == requested_) {
    return;
  }
  requested_ = on;
  failed_ = false;
  error_.clear();
  Q_EMIT stateChanged();
  retarget();
}

void NativeSurfaceHost::set_hdr_capable(bool on) {
  if (on == hdr_capable_) {
    return;
  }
  hdr_capable_ = on;
  failed_ = false;
  Q_EMIT stateChanged();
  retarget();
}

void NativeSurfaceHost::set_headroom(double headroom) {
  if (qFuzzyCompare(headroom, headroom_)) {
    return;
  }
  headroom_ = headroom;
  Q_EMIT stateChanged();
  retarget();
}

void NativeSurfaceHost::set_encoding(int encoding) {
  encoding = encoding == 1 ? 1 : 0;
  if (encoding == encoding_) {
    return;
  }
  encoding_ = encoding;
  failed_ = false;
  Q_EMIT stateChanged();
  retarget();
}

void NativeSurfaceHost::setVisible(bool visible) {
  if (visible == panel_visible_) {
    return;
  }
  panel_visible_ = visible;
  Q_EMIT stateChanged();
  retarget();
}

void NativeSurfaceHost::setGeometry(const QRect& rect) {
  if (rect == geometry_) {
    return;
  }
  geometry_ = rect;
  if (child_ != nullptr && !rect.isEmpty()) {
    child_->setGeometry(rect);
  }
  Q_EMIT geometryChanged();
  // The popup rule depends on what overlaps the rect.
  update_popup_state();
  retarget();
}

void NativeSurfaceHost::reportNativeFailure(const QString& error) {
  failed_ = true;
  error_ = error;
  Q_EMIT stateChanged();
  retarget();
}

bool NativeSurfaceHost::suppressed() const {
  if (!requested_) {
    return false;
  }
  return popup_open_ || !panel_visible_ || item_ == nullptr || !item_->isVisible();
}

// ---- Step 2: geometry -------------------------------------------------------------------------------------------

// Connects the item and each of its ancestors (a SplitView drag moves an ancestor, not the item).
void NativeSurfaceHost::track_item() {
  for (const QMetaObject::Connection& c : std::as_const(item_connections_)) {
    disconnect(c);
  }
  item_connections_.clear();
  QWindow* window = item_ != nullptr ? item_->window() : nullptr;
  if (window != parent_window_) {
    // Another window (a panel floated or moved to the HDR monitor window): a new child window there.
    if (child_ != nullptr) {
      child_->hide();
      child_->deleteLater();
      child_ = nullptr;
      handle_ = nullptr;
    }
    disconnect(exposed_connection_);
    waiting_for_expose_ = false;
    parent_window_ = window;
    failed_ = false;
  }
  if (item_ != nullptr) {
    item_connections_.append(connect(item_, &QQuickItem::windowChanged, this, &NativeSurfaceHost::track_item));
    item_connections_.append(connect(item_, &QQuickItem::visibleChanged, this, &NativeSurfaceHost::retarget));
    for (QQuickItem* it = item_; it != nullptr; it = it->parentItem()) {
      item_connections_.append(connect(it, &QQuickItem::xChanged, this, &NativeSurfaceHost::sync_geometry));
      item_connections_.append(connect(it, &QQuickItem::yChanged, this, &NativeSurfaceHost::sync_geometry));
      item_connections_.append(connect(it, &QQuickItem::widthChanged, this, &NativeSurfaceHost::sync_geometry));
      item_connections_.append(connect(it, &QQuickItem::heightChanged, this, &NativeSurfaceHost::sync_geometry));
      item_connections_.append(connect(it, &QQuickItem::parentChanged, this, &NativeSurfaceHost::track_item));
    }
  }
  if (parent_window_ != nullptr) {
    // A new screen may have another devicePixelRatio (drawable size) and EDR headroom.
    item_connections_.append(connect(parent_window_, &QWindow::screenChanged, this, &NativeSurfaceHost::retarget));
  }
  sync_geometry();
  retarget();
}

void NativeSurfaceHost::sync_geometry() {
  if (item_ == nullptr || item_->window() == nullptr) {
    setGeometry({});
    return;
  }
  setGeometry(mappedRect(*item_));
}

// ---- Step 3: popup rule -----------------------------------------------------------------------------------------

void NativeSurfaceHost::update_popup_state() {
  // Re-connect the overlay's children: a popup item shows, hides or moves without the child list changing.
  const qsizetype keep = overlay_ != nullptr ? 1 : 0;
  while (overlay_connections_.size() > keep) {
    disconnect(overlay_connections_.takeLast());
  }
  bool open = false;
  if (overlay_ != nullptr) {
    const QList<QQuickItem*> children = overlay_->childItems();
    for (QQuickItem* child : children) {
      for (auto signal : {&QQuickItem::visibleChanged, &QQuickItem::xChanged, &QQuickItem::yChanged,
                          &QQuickItem::widthChanged, &QQuickItem::heightChanged}) {
        overlay_connections_.append(connect(child, signal, this, &NativeSurfaceHost::update_popup_state));
      }
      // Only what can be drawn over the picture counts: a tooltip on the toolbar leaves the picture alone, a menu
      // dropped over it or a modal dimmer does not (the overlay fills the window: its coordinates are the scene's).
      if (child->isVisible() && child->width() > 0 && child->height() > 0) {
        const QRect r = mappedRect(*child);
        if (geometry_.isEmpty() || r.intersects(geometry_)) {
          open = true;
        }
      }
    }
  }
  if (open == popup_open_) {
    return;
  }
  popup_open_ = open;
  Q_EMIT stateChanged();
  retarget();
}

// ---- Step 1: lifecycle, and the switch ----------------------------------------------------------------------------

bool NativeSurfaceHost::want_native() const {
  return backend_ != nullptr && requested_ && hdr_capable_ && !failed_ && !suppressed() && !geometry_.isEmpty() &&
         parent_window_ != nullptr;
}

void NativeSurfaceHost::retarget() {
  if (want_native()) {
    QString why;
    if (ensure_child(why)) {
      child_->setGeometry(geometry_);
      if (!child_->isVisible()) {
        child_->show();
        child_->raise();  // above any sibling native window (another panel's, a floating dock's)
      }
      refresh_.start();
      publish(true);
      return;
    }
    if (!why.isEmpty() && why != error_) {
      error_ = why;
      Q_EMIT stateChanged();
    }
  }
  if (child_ != nullptr && child_->isVisible()) {
    child_->hide();
  }
  refresh_.stop();
  publish(false);
}

// Creates the child window once the parent window is exposed; false while it cannot (yet) be had.
bool NativeSurfaceHost::ensure_child(QString& why) {
  if (child_ != nullptr && handle_ != nullptr) {
    return true;
  }
  if (parent_window_ == nullptr) {
    why = tr("the Picture panel has no window");
    return false;
  }
  if (!parent_window_->isExposed()) {
    // winId() before the parent is exposed creates the native window too early (§10.11 WI-1.406 pitfalls): retry
    // at the parent's next frame.
    if (auto* quick = qobject_cast<QQuickWindow*>(parent_window_.data()); quick != nullptr && !waiting_for_expose_) {
      waiting_for_expose_ = true;
      exposed_connection_ = connect(quick, &QQuickWindow::afterAnimating, this, [this] {
        disconnect(exposed_connection_);
        waiting_for_expose_ = false;
        retarget();
      });
    }
    return false;
  }
  if (child_ == nullptr) {
    child_ = new QWindow(parent_window_.data());  // NOLINT(cppcoreguidelines-owning-memory): deleted in ~ or on move
    child_->setSurfaceType(backend_->surface_type());
    // The picture's input stays with the panel's MouseArea under the child window.
    child_->setFlags(Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    child_->setGeometry(geometry_);
    child_->create();
  }
  handle_ = backend_->native_handle(*child_, why);
  if (handle_ == nullptr) {
    failed_ = true;
    if (why.isEmpty()) {
      why = tr("the platform gave no native surface for the HDR window");
    }
    return false;
  }
  return true;
}

void NativeSurfaceHost::publish(bool native) {
  NativeTargetDesc next;
  next.native = native;
  if (native) {
    const double dpr = parent_window_ != nullptr ? parent_window_->devicePixelRatio() : 1.0;
    next.kind = backend_->target_kind();
    next.handle = handle_;
    next.device_pixel_ratio = dpr;
    next.width = static_cast<int>(std::lround(geometry_.width() * dpr));
    next.height = static_cast<int>(std::lround(geometry_.height() * dpr));
    next.hdr_capable = hdr_capable_;
    next.headroom = headroom_;
    next.encoding = encoding_;
  }
  if (same_target(next, target_)) {
    return;
  }
  next.generation = target_.generation + 1;
  target_ = next;
  Q_EMIT stateChanged();
  Q_EMIT targetChanged();
}

}  // namespace ovt::app
