// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "picture_controller.h"

#include <QJSValueList>

#include <algorithm>
#include <array>
#include <cmath>

namespace ovt::app {

namespace {

constexpr std::array<const char*, 5> kPlaneNames = {"YUV", "Y", "U", "V", "Gray"};
constexpr double kEpsilon = 1e-9;

bool same(double a, double b) {
  return std::abs(a - b) <= kEpsilon * std::max({1.0, std::abs(a), std::abs(b)});
}

int floor_to(int v, int step) {
  return v >= 0 ? (v / step) * step : -(((-v) + step - 1) / step) * step;
}

int ceil_to(int v, int step) {
  return -floor_to(-v, step);
}

QString percent(double zoom) {
  const double pct = zoom * 100.0;
  // 1/32 = 3.125 %: one decimal below 10 %, whole numbers above.
  return pct < 10.0 ? QString::number(pct, 'f', 1) + QStringLiteral(" %")
                    : QString::number(std::lround(pct)) + QStringLiteral(" %");
}

}  // namespace

PictureController::PictureController(QObject* parent) : QObject(parent) {
  // Zoom and pan move the picture under a resting pointer: the hovered sample and the read-out follow.
  connect(this, &PictureController::viewChanged, this, [this] {
    if (pointer_inside_) {
      const QPoint at = pick(pointer_.x(), pointer_.y());
      if (at != hover_) {
        hover_ = at;
        Q_EMIT hoverChanged();
      }
    }
    update_readout();
  });
  update_readout();
}

PictureController::~PictureController() = default;

// ---- Picture ----------------------------------------------------------------------------------------------------

void PictureController::setPicture(int coded_width, int coded_height, const QRect& crop) {
  coded_width = std::max(0, coded_width);
  coded_height = std::max(0, coded_height);
  if (coded_width == 0 || coded_height == 0) {
    coded_width = 0;
    coded_height = 0;
  }
  const QRect valid_crop = crop.isValid() ? crop.intersected(QRect(0, 0, coded_width, coded_height)) : QRect();
  if (coded_width == coded_w_ && coded_height == coded_h_ && valid_crop == crop_) {
    update_readout();  // a new frame of the same size: the code values under the pointer changed
    return;
  }
  const bool new_size = coded_width != coded_w_ || coded_height != coded_h_;
  coded_w_ = coded_width;
  coded_h_ = coded_height;
  crop_ = valid_crop;
  Q_EMIT pictureChanged();
  if (new_size || fit_) {
    fit_ = true;
    refit();
  } else {
    clamp_pan();
  }
  update_readout();
}

// ---- Zoom and pan -----------------------------------------------------------------------------------------------

PictureController::Visible PictureController::visible() const {
  if (show_cropped_ && crop_.width() > 0 && crop_.height() > 0) {
    return {static_cast<double>(crop_.x()), static_cast<double>(crop_.y()), static_cast<double>(crop_.width()),
            static_cast<double>(crop_.height())};
  }
  if (!has_picture()) {
    return {};
  }
  return {0.0, 0.0, static_cast<double>(coded_w_), static_cast<double>(coded_h_)};
}

void PictureController::refit() {
  // The same rule as PictureHost's zoom 0 (app/src/picture_host.cpp view_for()).
  const Visible v = visible();
  double zoom = 1.0;
  if (vw_ > 0 && vh_ > 0) {
    zoom = std::min(vw_ / v.w, vh_ / v.h);
  }
  const double pan_x = v.x + (v.w / 2.0);
  const double pan_y = v.y + (v.h / 2.0);
  if (same(zoom, zoom_) && same(pan_x, pan_x_) && same(pan_y, pan_y_)) {
    return;
  }
  zoom_ = zoom;
  pan_x_ = pan_x;
  pan_y_ = pan_y;
  Q_EMIT viewChanged();
}

void PictureController::clamp_pan() {
  if (fit_) {
    refit();
    return;
  }
  const double before_x = pan_x_;
  const double before_y = pan_y_;
  apply(zoom_, pan_x_, pan_y_);
  if (!same(before_x, pan_x_) || !same(before_y, pan_y_)) {
    update_readout();
  }
}

void PictureController::apply(double zoom, double pan_x, double pan_y) {
  zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
  // §11.3.2: pan clamps so that at least kMinVisible device pixels of the picture stay visible on each axis (fewer
  // when the picture or the viewport is smaller). With device = (image − pan) × zoom + w/2, the picture's right edge
  // stays at or right of m and its left edge at or left of w − m.
  const Visible v = visible();
  auto clamp_axis = [zoom](double pan, double origin, double extent, double viewport) {
    if (viewport <= 0) {
      return pan;
    }
    const double m = std::min({kMinVisible, extent * zoom, viewport});
    const double lo = origin + ((m - (viewport / 2.0)) / zoom);
    const double hi = origin + extent - ((m - (viewport / 2.0)) / zoom);
    return lo <= hi ? std::clamp(pan, lo, hi) : origin + (extent / 2.0);
  };
  pan_x = clamp_axis(pan_x, v.x, v.w, vw_);
  pan_y = clamp_axis(pan_y, v.y, v.h, vh_);
  const bool changed = fit_ || !same(zoom, zoom_) || !same(pan_x, pan_x_) || !same(pan_y, pan_y_);
  fit_ = false;
  zoom_ = zoom;
  pan_x_ = pan_x;
  pan_y_ = pan_y;
  if (changed) {
    Q_EMIT viewChanged();
  }
}

void PictureController::zoomAbout(double zoom, double px, double py) {
  if (!has_picture()) {
    return;
  }
  zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
  const QPointF at = imagePoint(px, py);
  apply(zoom, at.x() - ((px - (vw_ / 2.0)) / zoom), at.y() - ((py - (vh_ / 2.0)) / zoom));
  update_readout();
}

void PictureController::wheel(double px, double py, double angle_delta_y) {
  if (angle_delta_y == 0.0) {
    return;
  }
  // 2^(Δ/120): one notch is ×2; smooth-scrolling deltas of a notch's worth multiply to the same factor. Applying
  // each event at once is the coalescing: the host renders once per frame whatever the number of ViewState changes.
  zoomAbout(zoom_ * std::exp2(angle_delta_y / kNotch), px, py);
}

void PictureController::zoomIn() {
  // The next power of two strictly above the current zoom.
  const double level = std::exp2(std::floor(std::log2(zoom_) + kEpsilon) + 1.0);
  zoomTo(level);
}

void PictureController::zoomOut() {
  const double level = std::exp2(std::ceil(std::log2(zoom_) - kEpsilon) - 1.0);
  zoomTo(level);
}

void PictureController::zoomTo(double zoom) {
  zoomAbout(zoom, vw_ / 2.0, vh_ / 2.0);
}

void PictureController::fitToView() {
  if (fit_) {
    return;
  }
  fit_ = true;
  refit();
  Q_EMIT viewChanged();
  update_readout();
}

void PictureController::panBy(double dx, double dy) {
  if (!has_picture() || (dx == 0.0 && dy == 0.0)) {
    return;
  }
  apply(zoom_, pan_x_ - (dx / zoom_), pan_y_ - (dy / zoom_));
  update_readout();
}

void PictureController::beginPan(double px, double py) {
  pan_last_ = QPointF(px, py);
  if (!panning_) {
    panning_ = true;
    Q_EMIT dragChanged();
  }
}

void PictureController::movePan(double px, double py) {
  if (!panning_) {
    return;
  }
  const QPointF d = QPointF(px, py) - pan_last_;
  pan_last_ = QPointF(px, py);
  panBy(d.x(), d.y());
}

void PictureController::endPan() {
  if (panning_) {
    panning_ = false;
    Q_EMIT dragChanged();
  }
}

// ---- Mapping ----------------------------------------------------------------------------------------------------

QPointF PictureController::imagePoint(double px, double py) const {
  return {pan_x_ + ((px - (vw_ / 2.0)) / zoom_), pan_y_ + ((py - (vh_ / 2.0)) / zoom_)};
}

QPoint PictureController::pick(double px, double py) const {
  if (!has_picture()) {
    return {-1, -1};
  }
  const QPointF p = imagePoint(px, py);
  const Visible v = visible();
  if (p.x() < v.x || p.y() < v.y || p.x() >= v.x + v.w || p.y() >= v.y + v.h) {
    return {-1, -1};
  }
  return {static_cast<int>(std::floor(p.x())), static_cast<int>(std::floor(p.y()))};
}

QPointF PictureController::devicePoint(double ix, double iy) const {
  return {((ix - pan_x_) * zoom_) + (vw_ / 2.0), ((iy - pan_y_) * zoom_) + (vh_ / 2.0)};
}

// ---- Planes -----------------------------------------------------------------------------------------------------

void PictureController::set_plane_mode(int mode) {
  mode = std::clamp(mode, 0, static_cast<int>(kPlaneNames.size()) - 1);
  if (mode != plane_mode_) {
    plane_mode_ = mode;
    Q_EMIT planeChanged();
    update_readout();
  }
}

void PictureController::set_stage(int stage) {
  stage = std::clamp(stage, 0, 5);
  if (stage != stage_) {
    stage_ = stage;
    Q_EMIT planeChanged();
  }
}

void PictureController::set_stage_available(bool available) {
  if (available != stage_available_) {
    stage_available_ = available;
    Q_EMIT planeChanged();
  }
}

void PictureController::set_show_cropped(bool on) {
  if (on != show_cropped_) {
    show_cropped_ = on;
    Q_EMIT viewChanged();
    clamp_pan();
  }
}

// ---- Probe and selection rectangle --------------------------------------------------------------------------------

QPoint PictureController::probe() const {
  return selection_ != nullptr ? selection_->probe() : QPoint(-1, -1);
}

QRect PictureController::selection_rect() const {
  return selection_ != nullptr ? selection_->selection_rect() : QRect();
}

void PictureController::hoverAt(double px, double py) {
  pointer_ = QPointF(px, py);
  pointer_inside_ = true;
  const QPoint at = pick(px, py);
  if (at != hover_) {
    hover_ = at;
    Q_EMIT hoverChanged();
    update_readout();
  }
}

void PictureController::leave() {
  pointer_inside_ = false;
  if (hover_ != QPoint(-1, -1)) {
    hover_ = QPoint(-1, -1);
    Q_EMIT hoverChanged();
    update_readout();
  }
}

void PictureController::toggleProbe(double px, double py) {
  if (selection_ == nullptr) {
    return;
  }
  const QPoint at = pick(px, py);
  if (at.x() < 0) {
    return;
  }
  selection_->setProbe(at == probe() ? QPoint(-1, -1) : at, origin_);
}

void PictureController::moveProbe(int dx, int dy) {
  const QPoint p = probe();
  if (selection_ == nullptr || p.x() < 0 || !has_picture()) {
    return;
  }
  const QPoint next(std::clamp(p.x() + dx, 0, coded_w_ - 1), std::clamp(p.y() + dy, 0, coded_h_ - 1));
  if (next != p) {
    selection_->setProbe(next, origin_);
  }
}

QRect PictureController::snapped_rect(QPointF a, QPointF b) const {
  // Luma samples, outward to the 4 x 4 cell grid, inside the coded picture.
  const int x0 = floor_to(static_cast<int>(std::floor(std::min(a.x(), b.x()))), kCell);
  const int y0 = floor_to(static_cast<int>(std::floor(std::min(a.y(), b.y()))), kCell);
  const int x1 = ceil_to(static_cast<int>(std::ceil(std::max(a.x(), b.x()))), kCell);
  const int y1 = ceil_to(static_cast<int>(std::ceil(std::max(a.y(), b.y()))), kCell);
  const QRect r = QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1)).intersected(QRect(0, 0, coded_w_, coded_h_));
  return r.isValid() ? r : QRect();
}

void PictureController::beginRect(double px, double py) {
  if (!has_picture()) {
    return;
  }
  rect_anchor_ = imagePoint(px, py);
  drawing_rect_ = true;
  drag_rect_ = QRect();
  Q_EMIT rectChanged();
}

void PictureController::moveRect(double px, double py) {
  if (!drawing_rect_) {
    return;
  }
  const QRect r = snapped_rect(rect_anchor_, imagePoint(px, py));
  if (r != drag_rect_) {
    drag_rect_ = r;
    Q_EMIT rectChanged();
  }
}

void PictureController::endRect() {
  if (!drawing_rect_) {
    return;
  }
  drawing_rect_ = false;
  const QRect r = drag_rect_;
  drag_rect_ = QRect();
  if (selection_ != nullptr) {
    selection_->setRect(r, origin_);  // emits rectChanged through on_selection_changed
  }
  Q_EMIT rectChanged();
}

bool PictureController::escape() {
  if (drawing_rect_) {
    drawing_rect_ = false;
    drag_rect_ = QRect();
    Q_EMIT rectChanged();
    return true;
  }
  if (selection_ == nullptr || (probe().x() < 0 && selection_rect().isNull())) {
    return false;
  }
  if (probe().x() >= 0) {
    selection_->setProbe(QPoint(-1, -1), origin_);
  }
  if (!selection_rect().isNull()) {
    selection_->setRect(QRect(), origin_);
  }
  return true;
}

// ---- Actions ----------------------------------------------------------------------------------------------------

bool PictureController::handleAction(const QString& id) {
  static const std::array<const char*, 5> kPlaneActions = {"view.plane.yuv", "view.plane.y", "view.plane.u",
                                                           "view.plane.v", "view.plane.gray"};
  for (std::size_t i = 0; i < kPlaneActions.size(); ++i) {
    if (id == QLatin1String(kPlaneActions.at(i))) {
      set_plane_mode(static_cast<int>(i));
      return true;
    }
  }
  if (id == QLatin1String("view.zoom.in")) {
    zoomIn();
  } else if (id == QLatin1String("view.zoom.out")) {
    zoomOut();
  } else if (id == QLatin1String("view.zoom.fit")) {
    fitToView();
  } else if (id == QLatin1String("view.zoom.100")) {
    zoomTo(1.0);
  } else if (id == QLatin1String("view.zoom.200")) {
    zoomTo(2.0);
  } else if (id == QLatin1String("view.zoom.400")) {
    zoomTo(4.0);
  } else {
    return false;
  }
  return true;
}

// ---- Properties -------------------------------------------------------------------------------------------------

void PictureController::set_selection(SelectionModel* selection) {
  if (selection == selection_) {
    return;
  }
  if (selection_ != nullptr) {
    disconnect(selection_, nullptr, this, nullptr);
  }
  selection_ = selection;
  if (selection_ != nullptr) {
    connect(selection_, &SelectionModel::changed, this, &PictureController::on_selection_changed);
  }
  Q_EMIT selectionChanged();
  Q_EMIT probeChanged();
  Q_EMIT rectChanged();
  update_readout();
}

void PictureController::set_origin(const QString& origin) {
  if (origin != origin_) {
    origin_ = origin;
    Q_EMIT selectionChanged();
  }
}

void PictureController::set_viewport_width(double w) {
  w = std::max(0.0, w);
  if (!same(w, vw_)) {
    vw_ = w;
    clamp_pan();
    Q_EMIT viewChanged();
  }
}

void PictureController::set_viewport_height(double h) {
  h = std::max(0.0, h);
  if (!same(h, vh_)) {
    vh_ = h;
    clamp_pan();
    Q_EMIT viewChanged();
  }
}

void PictureController::set_sample_reader(const QJSValue& reader) {
  sample_reader_ = reader;
  Q_EMIT hoverChanged();
  update_readout();
}

void PictureController::on_selection_changed(int mask) {
  const auto bits = static_cast<unsigned>(mask);
  if ((bits & SelectionModel::ChangeProbe) != 0U) {
    Q_EMIT probeChanged();
  }
  if ((bits & SelectionModel::ChangeRect) != 0U) {
    Q_EMIT rectChanged();
  }
  if ((bits & SelectionModel::ChangeFrame) != 0U && selection_ != nullptr) {
    Q_EMIT frameWanted(selection_->frame_decode_index(), selection_->frame_display_index());
  }
  if ((bits & (SelectionModel::ChangeProbe | SelectionModel::ChangeFrame)) != 0U) {
    update_readout();
  }
}

void PictureController::update_readout() {
  // Hover read-out; the pinned probe's when the pointer is off the picture (the full Pick is in Selection Info).
  QPoint at = hover_;
  QString prefix;
  if (at.x() < 0 && probe().x() >= 0) {
    at = probe();
    prefix = QStringLiteral("Probe ");
  }
  QString text;
  if (at.x() >= 0) {
    text = prefix + QStringLiteral("x %1, y %2").arg(at.x()).arg(at.y());
    if (sample_reader_.isCallable()) {
      const QJSValue codes = sample_reader_.call(QJSValueList{at.x(), at.y()});
      const int n = codes.isArray() ? codes.property(QStringLiteral("length")).toInt() : 0;
      static constexpr std::array<const char*, 3> kComponents = {"Y", "U", "V"};
      for (int i = 0; i < std::min(n, 3); ++i) {
        const QJSValue c = codes.property(static_cast<quint32>(i));
        if (c.isNumber() && c.toInt() >= 0) {
          text += (i == 0 ? QStringLiteral(" · ") : QStringLiteral(" ")) +
                  QLatin1String(kComponents.at(static_cast<std::size_t>(i))) + QLatin1Char(' ') +
                  QString::number(c.toInt());
        }
      }
    }
    text += QStringLiteral(" · ");
  }
  if (has_picture()) {
    text += (fit_ ? QStringLiteral("Fit ") + percent(zoom_) : percent(zoom_)) + QStringLiteral(" · ") +
            QLatin1String(kPlaneNames.at(static_cast<std::size_t>(plane_mode_)));
  }
  if (text != readout_) {
    readout_ = text;
    Q_EMIT readoutChanged();
  }
}

}  // namespace ovt::app
