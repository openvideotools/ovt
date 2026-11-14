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
enum Layer : int { Band = 0, Bars = 1, Hatch = 2, Mean = 3, Arcs = 4, Ticks = 5, Marker = 6 };

constexpr Rgba kHatchColor{0, 0, 0, 150};
constexpr Rgba kTickColor{0xE5, 0x39, 0x35, 255};
constexpr Rgba kArcColor{0xFF, 0x85, 0x1B, 150};       // SPEC-ASSUMPTION(#402): all arrows L0 orange, never dashed
constexpr Rgba kArcHoverColor{0xFF, 0xD7, 0x00, 255};  // hover gold
constexpr std::size_t kMaxArcs = 3000;
constexpr float kTickHeight = 6.0F;

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
    connect(model_, &StreamIndexModel::orderChanged, this, [this] { view_changed(); });
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

void TimelineChart::set_ref_arrows(bool on) {
  if (ref_arrows_ != on) {
    ref_arrows_ = on;
    Q_EMIT refArrowsChanged();
    update();
  }
}

QVariantList TimelineChart::gop_markers() const {
  QVariantList out;
  if (model_.isNull() || width() <= 0.0) {
    return out;
  }
  const FrameTable& table = model_->frames();
  for (const std::size_t position : table.rap_positions()) {
    if (position < view_.first_frame() || position >= view_.end_frame()) {
      continue;
    }
    const int decode = table.decode_index_at(position);
    QVariantMap marker;
    marker.insert(QStringLiteral("x"), view_.x_of(static_cast<double>(position) + 0.5, width()));
    marker.insert(QStringLiteral("label"),
                  table.type(decode) == fb::FrameType::Key ? QStringLiteral("K") : QStringLiteral("I"));
    out.append(marker);
  }
  return out;
}

double TimelineChart::xOfFrame(int decode_index) const {
  if (model_.isNull() || width() <= 0.0) {
    return -1.0;
  }
  const int position = model_->positionOf(decode_index);
  if (position < 0 || static_cast<double>(position) < view_.first_frame() ||
      static_cast<double>(position) >= view_.end_frame()) {
    return -1.0;
  }
  return view_.x_of(static_cast<double>(position) + 0.5, width());
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
  Q_EMIT viewChanged();  // the marker x positions depend on the width
  update();
}

QSGNode* TimelineChart::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/) {
  auto* root = old_node;
  if (root == nullptr) {
    root = new QSGNode;
    root->appendChildNode(make_layer(QSGGeometry::DrawTriangles));  // Band
    root->appendChildNode(make_layer(QSGGeometry::DrawTriangles));  // Bars
    root->appendChildNode(make_layer(QSGGeometry::DrawLines));      // Hatch
    root->appendChildNode(make_layer(QSGGeometry::DrawLines));      // Mean
    root->appendChildNode(make_layer(QSGGeometry::DrawLines));      // Arcs
    root->appendChildNode(make_layer(QSGGeometry::DrawTriangles));  // Ticks
    root->appendChildNode(make_layer(QSGGeometry::DrawTriangles));  // Marker
  }
  const LayerVertices layers = build_layers(static_cast<float>(width()), static_cast<float>(height()));
  int i = 0;
  for (QSGNode* child = root->firstChild(); child != nullptr && i < LayerCount; child = child->nextSibling(), ++i) {
    fill_layer(static_cast<QSGGeometryNode*>(child), layers.at(static_cast<std::size_t>(i)));
  }
  return root;
}

TimelineChart::LayerVertices TimelineChart::build_layers(float w, float h) {
  LayerVertices layers;
  max_value_ = 0.0;
  if (model_.isNull() || model_->frames().size() == 0 || !(w > 0.0F) || !(h > 0.0F)) {
    return layers;
  }
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
    colors[c] = has_rap ? frame_type_color(fb::FrameType::I) : frame_type_color(table.type(decode));
  }
  layers[Bars] = build_bars(columns, colors, max_value_, w, h);
  layers[Hatch] = hatch_vertices(columns, w, h);
  if (mean_line_) {
    layers[Mean] = build_mean_line(columns, max_value_, w, h, kMeanColor);
  }
  layers[Arcs] = arc_vertices(w, h);
  // GOP ticks at the random access points in view.
  std::vector<float> tick_xs;
  for (const std::size_t rap : raps) {
    if (rap >= first && rap < end) {
      tick_xs.push_back(x_of_position(static_cast<double>(rap) + 0.5, w));
    }
  }
  layers[Ticks] = build_ticks(tick_xs, h, kTickHeight, kTickColor);
  if (model_->has_range()) {
    const float x0 = x_of_position(static_cast<double>(model_->range_first()), w);
    const float x1 = x_of_position(static_cast<double>(model_->range_last()) + 1.0, w);
    layers[Band] = build_rect(std::max(x0, 0.0F), 0.0F, std::min(x1, w), h, kRangeBand);
  }
  if (const auto selected = table.position_of(selected_frame_)) {
    const float x0 = x_of_position(static_cast<double>(*selected), w);
    const float x1 = x_of_position(static_cast<double>(*selected) + 1.0, w);
    if (x1 > 0.0F && x0 < w) {
      layers[Marker] = build_rect(x0, h - 3.0F, std::max(x1, x0 + 2.0F), h, kSelected);
    }
  }
  return layers;
}

float TimelineChart::x_of_position(double position, float w) const {
  return static_cast<float>(view_.x_of(position, static_cast<double>(w)));
}

// Hidden frames (AV1 shown == false) are hatched; only where a column is a single frame at least 3 px wide.
std::vector<ColorVertex> TimelineChart::hatch_vertices(const std::vector<SeriesColumn>& columns, float w,
                                                       float h) const {
  std::vector<ColorVertex> out;
  const float column_width = w / static_cast<float>(columns.size());
  if (column_width < 3.0F || !(max_value_ > 0.0)) {
    return out;
  }
  const FrameTable& table = model_->frames();
  for (std::size_t c = 0; c < columns.size(); ++c) {
    const bool single = columns[c].last - columns[c].first == 1;
    if (!single || table.shown(table.decode_index_at(columns[c].first)) || !(columns[c].max > 0.0)) {
      continue;
    }
    const float x0 = static_cast<float>(c) * column_width;
    const auto bar_height = static_cast<float>(std::min(columns[c].max / max_value_, 1.0)) * h;
    const auto lines = build_hatch(x0, h - bar_height, x0 + column_width - 1.0F, h, kHatchColor);
    out.insert(out.end(), lines.begin(), lines.end());
  }
  return out;
}

// Reference arrows, when a frame is wide enough to tell them apart.
std::vector<ColorVertex> TimelineChart::arc_vertices(float w, float h) const {
  if (!ref_arrows_ || static_cast<double>(w) / view_.count() < kMinArrowFramePixels) {
    return {};
  }
  const FrameTable& table = model_->frames();
  std::vector<RefArc> refs;
  for (std::size_t p = view_.first_frame(); p < view_.end_frame() && refs.size() < kMaxArcs; ++p) {
    const bool hovered = static_cast<int>(p) == hover_position_;
    for (const int ref : table.declared_refs(table.decode_index_at(p))) {
      if (const auto target = table.position_of(ref)) {
        refs.push_back(RefArc{x_of_position(static_cast<double>(p) + 0.5, w),
                              x_of_position(static_cast<double>(*target) + 0.5, w),
                              hovered ? kArcHoverColor : kArcColor});
      }
    }
  }
  return build_ref_arcs(refs, h - kTickHeight, h / 2.0F);
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
  const int position = positionAt(event->position().x());
  if (position != hover_position_) {
    hover_position_ = position;
    if (ref_arrows_) {
      update();  // the hovered frame's arrows are highlighted
    }
  }
  Q_EMIT hovered(position, event->position().x());
}

void TimelineChart::hoverLeaveEvent(QHoverEvent* /*event*/) {
  hover_position_ = -1;
  update();
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
