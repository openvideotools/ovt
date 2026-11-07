// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "items/timeline_chart.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "models/timeline_geometry.h"

namespace ovt::app {

namespace {

constexpr Rgba kRangeBand{0x39, 0xCC, 0xCC, 64};  // #39CCCC 25 %
constexpr Rgba kSelected{0xFF, 0xD7, 0x00, 255};  // hover / current: #FFD700
constexpr Rgba kMeanColor{0xFF, 0xFF, 0xFF, 220};

// A node with vertex-colour material that is refilled each paint.
QSGGeometryNode* make_layer(unsigned int drawing_mode) {
  auto* node = new QSGGeometryNode;
  auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
  geometry->setDrawingMode(drawing_mode);
  auto* material = new QSGVertexColorMaterial;
  material->setFlag(QSGMaterial::Blending);
  node->setGeometry(geometry);
  node->setMaterial(material);
  node->setFlag(QSGNode::OwnsGeometry);
  node->setFlag(QSGNode::OwnsMaterial);
  return node;
}

void fill_layer(QSGGeometryNode* node, const std::vector<ColorVertex>& vertices) {
  QSGGeometry* geometry = node->geometry();
  geometry->allocate(static_cast<int>(vertices.size()));
  if (!vertices.empty()) {
    static_assert(sizeof(ColorVertex) == sizeof(QSGGeometry::ColoredPoint2D));
    std::memcpy(geometry->vertexData(), vertices.data(), vertices.size() * sizeof(ColorVertex));
  }
  node->markDirty(QSGNode::DirtyGeometry);
}

// The children of the root node, bottom to top.
enum Layer : int { Band = 0, Bars = 1, Mean = 2, Marker = 3, LayerCount = 4 };

}  // namespace

TimelineChart::TimelineChart(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setFlag(ItemIsFocusScope, false);
  setAcceptedMouseButtons(Qt::LeftButton);
  setAcceptHoverEvents(true);
  setActiveFocusOnTab(true);
}

TimelineChart::~TimelineChart() = default;

void TimelineChart::set_model(StreamIndexModel* model) {
  if (model_ == model) {
    return;
  }
  if (!model_.isNull()) {
    disconnect(model_, nullptr, this, nullptr);
  }
  model_ = model;
  if (!model_.isNull()) {
    connect(model_, &StreamIndexModel::indexChanged, this, &TimelineChart::on_index_changed);
    connect(model_, &StreamIndexModel::orderChanged, this, [this] { update(); });
    connect(model_, &StreamIndexModel::rangeChanged, this, [this] { update(); });
  }
  on_index_changed();
  Q_EMIT modelChanged();
}

void TimelineChart::on_index_changed() {
  view_.set_total(!model_.isNull() ? model_->frames().size() : 0);
  view_changed();
}

void TimelineChart::view_changed() {
  Q_EMIT viewChanged();
  update();
}

void TimelineChart::set_series(int series) {
  const auto kind = static_cast<Series>(std::clamp(series, 0, 3));
  if (series_.kind != kind) {
    series_.kind = kind;
    Q_EMIT seriesChanged();
    update();
  }
}

void TimelineChart::set_metric_id(const QString& id) {
  const std::string value = id.toStdString();
  if (series_.metric_id != value) {
    series_.metric_id = value;
    Q_EMIT seriesChanged();
    update();
  }
}

void TimelineChart::set_plane(int plane) {
  if (series_.plane != plane) {
    series_.plane = plane;
    Q_EMIT seriesChanged();
    update();
  }
}

void TimelineChart::set_mean_line(bool on) {
  if (mean_line_ != on) {
    mean_line_ = on;
    Q_EMIT meanLineChanged();
    update();
  }
}

void TimelineChart::set_selected_frame(int decode_index) {
  if (selected_frame_ != decode_index) {
    selected_frame_ = decode_index;
    Q_EMIT selectedFrameChanged();
    update();
  }
}

void TimelineChart::fit() {
  view_.fit();
  view_changed();
}

void TimelineChart::zoomAbout(double notches, double x) {
  view_.zoom_notches(notches, width() > 0 ? x / width() : 0.0);
  view_changed();
}

void TimelineChart::pan(double frames) {
  view_.pan_frames(frames);
  view_changed();
}

int TimelineChart::positionAt(double x) const {
  return !model_.isNull() && model_->frames().size() != 0 ? static_cast<int>(view_.frame_at(x, width())) : -1;
}

QString TimelineChart::frameTooltip(int position) const {
  if (model_.isNull() || position < 0) {
    return {};
  }
  const FrameTable& t = model_->frames();
  const int decode = t.decode_index_at(static_cast<std::size_t>(position));
  if (decode < 0) {
    return {};
  }
  static constexpr std::array<const char*, 8> kTypes{"I", "P", "B", "Key", "Inter", "IntraOnly", "Switch", "?"};
  const auto type = static_cast<std::size_t>(t.type(decode));
  return QStringLiteral("#%1/#%2 · %3 · %4 B · QP %5 · Tid %6 · POC %7 · pts %8")
      .arg(t.display_index_of(decode))
      .arg(decode)
      .arg(QString::fromLatin1(kTypes.at(std::min<std::size_t>(type, kTypes.size() - 1))))
      .arg(static_cast<qulonglong>(t.size_bytes(decode)))
      .arg(t.qp_base(decode))
      .arg(t.temporal_id(decode))
      .arg(t.poc(decode))
      .arg(t.pts(decode));
}

void TimelineChart::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
  QQuickItem::geometryChange(new_geometry, old_geometry);
  update();
}

QSGNode* TimelineChart::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/) {
  auto* root = old_node;
  if (root == nullptr) {
    root = new QSGNode;
    root->appendChildNode(make_layer(QSGGeometry::DrawTriangles));  // Band
    root->appendChildNode(make_layer(QSGGeometry::DrawTriangles));  // Bars
    root->appendChildNode(make_layer(QSGGeometry::DrawLines));      // Mean
    root->appendChildNode(make_layer(QSGGeometry::DrawTriangles));  // Marker
  }
  std::array<QSGGeometryNode*, LayerCount> layers{};
  int i = 0;
  for (QSGNode* child = root->firstChild(); child != nullptr && i < LayerCount; child = child->nextSibling(), ++i) {
    layers.at(static_cast<std::size_t>(i)) = static_cast<QSGGeometryNode*>(child);
  }

  const auto w = static_cast<float>(width());
  const auto h = static_cast<float>(height());
  std::vector<ColorVertex> bars;
  std::vector<ColorVertex> mean;
  std::vector<ColorVertex> band;
  std::vector<ColorVertex> marker;
  max_value_ = 0.0;
  if (model_ != nullptr && model_->frames().size() != 0 && w > 0.0F && h > 0.0F) {
    FrameTable& table = model_->frames();
    const std::size_t first = view_.first_frame();
    const std::size_t end = view_.end_frame();
    const auto pixels = static_cast<std::size_t>(std::max(1.0F, std::floor(w)));
    const std::vector<SeriesColumn> columns = table.aggregator(series_).columns(first, end, pixels);
    max_value_ = max_of(columns);
    // A column takes the colour of its first frame's type; a column that holds a random access point shows the
    // I colour instead, so the GOP structure survives at any zoom (§11.3.1).
    const std::vector<std::size_t> raps = table.rap_positions();
    std::vector<Rgba> colors(columns.size());
    for (std::size_t c = 0; c < columns.size(); ++c) {
      const int decode = table.decode_index_at(columns[c].first);
      const auto rap = std::lower_bound(raps.begin(), raps.end(), columns[c].first);
      const bool has_rap = rap != raps.end() && *rap < columns[c].last;
      colors[c] = has_rap ? frame_type_color(fb::FrameType::I, table.shown(decode))
                          : frame_type_color(table.type(decode), table.shown(decode));
    }
    bars = build_bars(columns, colors, max_value_, w, h);
    if (mean_line_) {
      mean = build_mean_line(columns, max_value_, w, h, kMeanColor);
    }
    const auto x_span = [&](std::size_t a, std::size_t b) {
      return std::pair{static_cast<float>(view_.x_of(static_cast<double>(a), w)),
                       static_cast<float>(view_.x_of(static_cast<double>(b), w))};
    };
    if (model_->has_range()) {
      const auto [x0, x1] =
          x_span(static_cast<std::size_t>(model_->range_first()), static_cast<std::size_t>(model_->range_last()) + 1);
      band = build_rect(std::max(x0, 0.0F), 0.0F, std::min(x1, w), h, kRangeBand);
    }
    if (const auto selected = table.position_of(selected_frame_)) {
      const auto [x0, x1] = x_span(*selected, *selected + 1);
      if (x1 > 0.0F && x0 < w) {
        marker = build_rect(x0, h - 3.0F, std::max(x1, x0 + 2.0F), h, kSelected);
      }
    }
  }
  fill_layer(layers[Band], band);
  fill_layer(layers[Bars], bars);
  fill_layer(layers[Mean], mean);
  fill_layer(layers[Marker], marker);
  return root;
}

void TimelineChart::mousePressEvent(QMouseEvent* event) {
  forceActiveFocus();
  pressed_ = true;
  dragging_ = false;
  press_x_ = event->position().x();
  press_position_ = positionAt(press_x_);
  event->accept();
}

void TimelineChart::mouseMoveEvent(QMouseEvent* event) {
  if (!pressed_ || model_ == nullptr) {
    return;
  }
  if (!dragging_ && std::abs(event->position().x() - press_x_) >= kDragThreshold) {
    dragging_ = true;
  }
  if (dragging_) {
    model_->setRange(press_position_, positionAt(event->position().x()));
  }
  event->accept();
}

void TimelineChart::mouseReleaseEvent(QMouseEvent* event) {
  if (pressed_ && !dragging_ && model_ != nullptr) {
    const int position = positionAt(event->position().x());
    const int decode = position >= 0 ? model_->frames().decode_index_at(static_cast<std::size_t>(position)) : -1;
    if (decode >= 0) {
      Q_EMIT frameClicked(decode, model_->frames().display_index_of(decode));
    }
  }
  pressed_ = false;
  dragging_ = false;
  event->accept();
}

void TimelineChart::mouseDoubleClickEvent(QMouseEvent* event) {
  fit();
  event->accept();
}

void TimelineChart::wheelEvent(QWheelEvent* event) {
  const double notches = event->angleDelta().y() / 120.0;
  if (notches == 0.0) {
    return;
  }
  if ((event->modifiers() & Qt::ShiftModifier) != 0) {
    // Shift+wheel pans by a fifth of the view per notch.
    pan(-notches * view_.count() / 5.0);
  } else {
    zoomAbout(notches, event->position().x());
  }
  event->accept();
}

void TimelineChart::hoverMoveEvent(QHoverEvent* event) {
  Q_EMIT hovered(positionAt(event->position().x()), event->position().x());
}

void TimelineChart::hoverLeaveEvent(QHoverEvent* /*event*/) {
  Q_EMIT hovered(-1, 0.0);
}

void TimelineChart::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Left) {
    Q_EMIT stepRequested(-1);
    event->accept();
  } else if (event->key() == Qt::Key_Right) {
    Q_EMIT stepRequested(1);
    event->accept();
  } else {
    QQuickItem::keyPressEvent(event);
  }
}

}  // namespace ovt::app
