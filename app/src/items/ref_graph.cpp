// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "items/ref_graph.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantMap>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "models/timeline_geometry.h"

namespace ovt::app {

namespace {

constexpr QRgb kArrowColor = 0xFFFB8C00;       // orange: references (L0 vs L1 is not in the index, §11.3.11)
constexpr QRgb kArrowHoverColor = 0xFFFFFFFF;  // the hovered frame's in and out arrows
constexpr int kWheelStep = 8;                  // columns per wheel notch

QColor to_qcolor(Rgba c) {
  return QColor(c.r, c.g, c.b, c.a);
}

}  // namespace

RefGraph::RefGraph(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAcceptedMouseButtons(Qt::LeftButton);
  setAcceptHoverEvents(true);
  setAntialiasing(true);
  layout_.params.axis = RefAxis::Display;
}

RefGraph::~RefGraph() = default;

RefAxis RefGraph::axis() const noexcept {
  return display_axis_ ? RefAxis::Display : RefAxis::Decode;
}

RefGraphGeometry RefGraph::geometry() const {
  return geometry_of(layout_, width(), height());
}

void RefGraph::set_model(StreamIndexModel* model) {
  if (model_ == model) {
    return;
  }
  if (!model_.isNull()) {
    disconnect(model_, nullptr, this, nullptr);
  }
  model_ = model;
  if (!model_.isNull()) {
    connect(model_, &StreamIndexModel::indexChanged, this, &RefGraph::on_index_changed);
  }
  on_index_changed();
  Q_EMIT modelChanged();
}

void RefGraph::on_index_changed() {
  relayout(true);
}

void RefGraph::set_row_mode(int mode) {
  const auto m = static_cast<RefRowMode>(std::clamp(mode, 0, 2));
  if (m != mode_) {
    mode_ = m;
    relayout(true);
  }
}

void RefGraph::set_display_axis(bool display) {
  if (display != display_axis_) {
    display_axis_ = display;
    relayout(true);
  }
}

void RefGraph::set_selected_frame(int decode_index) {
  if (decode_index == selected_frame_) {
    return;
  }
  selected_frame_ = decode_index;
  // Keep the window when the frame is still inside it; otherwise recentre on it.
  const bool inside = layout_.find(decode_index) != nullptr;
  relayout(!inside);
  Q_EMIT selectedFrameChanged();
}

void RefGraph::set_text_color(const QColor& c) {
  text_color_ = c;
  update();
  Q_EMIT styleChanged();
}

void RefGraph::set_outline_color(const QColor& c) {
  outline_color_ = c;
  update();
  Q_EMIT styleChanged();
}

void RefGraph::relayout(bool recentre) {
  RefGraphParams params;
  params.mode = mode_;
  params.axis = axis();
  params.first_column = layout_.params.first_column;
  if (model_.isNull() || model_->frames().size() == 0) {
    layout_ = RefGraphLayout{};
    layout_.params = params;
  } else {
    const FrameTable& table = model_->frames();
    const RefAxis effective = mode_ == RefRowMode::DecodeVsDisplay ? RefAxis::Decode : params.axis;
    if (recentre) {
      params.first_column = centered_first_column(table, effective, std::max(selected_frame_, 0), params.columns);
    }
    layout_ = layout_ref_graph(table, params);
  }
  update();
  Q_EMIT layoutChanged();
}

void RefGraph::scrollWindow(int frames) {
  if (model_.isNull()) {
    return;
  }
  const int total = static_cast<int>(model_->frames().size());
  const int target = std::clamp(layout_.params.first_column + frames, 0, std::max(0, total - layout_.params.columns));
  if (target == layout_.params.first_column) {
    return;
  }
  layout_.params.first_column = target;
  RefGraphParams params = layout_.params;
  params.mode = mode_;
  params.axis = axis();
  layout_ = layout_ref_graph(model_->frames(), params);
  update();
  Q_EMIT layoutChanged();
}

QVariantList RefGraph::nodes() const {
  QVariantList out;
  for (const RefNode& n : layout_.nodes) {
    QVariantMap m;
    m.insert(QStringLiteral("decodeIndex"), n.decode_index);
    m.insert(QStringLiteral("displayIndex"), n.display_index);
    m.insert(QStringLiteral("row"), n.row);
    m.insert(QStringLiteral("column"), n.column);
    m.insert(QStringLiteral("temporalId"), n.temporal_id);
    m.insert(QStringLiteral("refsUnknown"), n.refs_unknown);
    m.insert(QStringLiteral("mirror"), n.mirror);
    out.push_back(m);
  }
  return out;
}

QVariantList RefGraph::arrows() const {
  QVariantList out;
  for (const RefArrow& a : layout_.arrows) {
    QVariantMap m;
    m.insert(QStringLiteral("from"), a.from_decode);
    m.insert(QStringLiteral("to"), a.to_decode);
    m.insert(QStringLiteral("targetInWindow"), a.target_in_window);
    out.push_back(m);
  }
  return out;
}

int RefGraph::frameAt(double x, double y) const {
  const RefNode* n = node_at(layout_, geometry(), x, y, kHitRadius);
  return n != nullptr ? n->decode_index : -1;
}

QVariantMap RefGraph::nodePosition(int decode_index) const {
  QVariantMap out;
  if (const RefNode* n = layout_.find(decode_index)) {
    const RefGraphGeometry g = geometry();
    out.insert(QStringLiteral("x"), g.x_of(n->column, layout_.params.first_column));
    out.insert(QStringLiteral("y"), g.y_of(n->row));
  }
  return out;
}

QString RefGraph::frameTooltip(int decode_index) const {
  const RefNode* n = layout_.find(decode_index);
  if (n == nullptr) {
    return {};
  }
  QStringList refs;
  for (const RefArrow& a : layout_.arrows) {
    if (a.from_decode == decode_index) {
      refs.push_back(QStringLiteral("#%1").arg(a.to_decode));
    }
  }
  const QString ref_text = n->refs_unknown  ? tr("references unknown")
                           : refs.isEmpty() ? tr("no references")
                                            : tr("refs %1").arg(refs.join(QStringLiteral(", ")));
  return QStringLiteral("#%1/#%2 · %3 · POC %4 · T%5 · %6")
      .arg(n->display_index)
      .arg(n->decode_index)
      .arg(QString::fromLatin1(frame_type_label(n->type)))
      .arg(n->poc)
      .arg(n->temporal_id)
      .arg(ref_text);
}

QVariantList RefGraph::dpbChips(int decode_index) const {
  QVariantList out;
  if (model_.isNull() || decode_index < 0) {
    return out;
  }
  for (const int d : model_->frames().dpb_after(decode_index)) {
    QVariantMap m;
    m.insert(QStringLiteral("decodeIndex"), d);
    m.insert(QStringLiteral("label"), QStringLiteral("#%1").arg(d));
    out.push_back(m);
  }
  return out;
}

void RefGraph::copyFrameList() const {
  if (QClipboard* cb = QGuiApplication::clipboard()) {
    cb->setText(QString::fromStdString(frame_list_text(layout_)));
  }
}

void RefGraph::copySvg() const {
  if (QClipboard* cb = QGuiApplication::clipboard()) {
    cb->setText(QString::fromStdString(ref_graph_svg(layout_, width(), height())));
  }
}

void RefGraph::paint(QPainter* painter) {
  painter->setRenderHint(QPainter::Antialiasing);
  const RefGraphGeometry g = geometry();
  const int first = layout_.params.first_column;
  const double size = std::min({g.cell_width() * 0.8, g.row_height() * 0.8, 18.0});
  auto centre = [&](const RefNode& n) { return QPointF(g.x_of(n.column, first), g.y_of(n.row)); };

  // Row labels (T0, T1, ... or Decode / Display).
  painter->setPen(text_color_);
  for (int r = 0; r < layout_.rows; ++r) {
    QString label;
    if (layout_.params.mode == RefRowMode::TemporalLayers) {
      label = QStringLiteral("T%1").arg(r);
    } else if (layout_.params.mode == RefRowMode::DecodeVsDisplay) {
      label = r == 0 ? tr("Dec") : tr("Disp");
    }
    painter->drawText(QRectF(2, g.y_of(r) - 8, g.margin_left - 4, 16), Qt::AlignVCenter | Qt::AlignLeft, label);
  }

  // Decode-vs-display links.
  if (layout_.params.mode == RefRowMode::DecodeVsDisplay) {
    painter->setPen(QPen(QColor(outline_color_.red(), outline_color_.green(), outline_color_.blue(), 70), 1));
    for (const RefNode& n : layout_.nodes) {
      if (n.mirror) {
        if (const RefNode* p = layout_.find(n.decode_index)) {
          painter->drawLine(centre(*p), centre(n));
        }
      }
    }
  }

  // Arrows.
  const bool dim = hover_frame_ >= 0;
  for (const RefArrow& a : layout_.arrows) {
    const RefNode* from = layout_.find(a.from_decode);
    const RefNode* to = layout_.find(a.to_decode);
    if (from == nullptr || to == nullptr) {
      continue;
    }
    const bool hot = a.from_decode == hover_frame_ || a.to_decode == hover_frame_;
    QColor color(hot ? kArrowHoverColor : kArrowColor);
    color.setAlpha(hot || !dim ? 230 : 90);
    painter->setPen(QPen(color, hot ? 2 : 1));
    const QPointF p0 = centre(*from);
    const QPointF p1 = centre(*to);
    const double lift = std::min(std::abs(p1.x() - p0.x()) * 0.35 + 6.0, g.row_height());
    QPainterPath path(p0);
    path.quadTo(QPointF((p0.x() + p1.x()) / 2, std::min(p0.y(), p1.y()) - lift), p1);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
  }

  // Nodes: filled by frame type; an open node has no known references; a hidden frame is drawn at half opacity.
  QFont small = painter->font();
  small.setPixelSize(std::max(8, static_cast<int>(size * 0.6)));
  painter->setFont(small);
  for (const RefNode& n : layout_.nodes) {
    const QPointF c = centre(n);
    const QRectF box(c.x() - size / 2, c.y() - size / 2, size, size);
    const QColor type_color = to_qcolor(frame_type_color(n.type));
    painter->setOpacity(n.shown ? 1.0 : 0.55);
    if (n.refs_unknown) {
      painter->setPen(QPen(type_color, 1.5, Qt::DashLine));
      painter->setBrush(Qt::NoBrush);
    } else {
      painter->setPen(Qt::NoPen);
      painter->setBrush(type_color);
    }
    painter->drawRoundedRect(box, 2, 2);
    painter->setOpacity(1.0);
    if (size >= 11) {
      painter->setPen(n.refs_unknown ? text_color_ : QColor(Qt::white));
      painter->drawText(box, Qt::AlignCenter, QString::fromLatin1(frame_type_label(n.type)));
    }
    if (n.decode_index == selected_frame_) {
      painter->setPen(QPen(outline_color_, 2));
      painter->setBrush(Qt::NoBrush);
      painter->drawRoundedRect(box.adjusted(-2, -2, 2, 2), 3, 3);
    }
  }

  // Column labels: POC every few columns that fit.
  painter->setPen(text_color_);
  const int stride = std::max(1, static_cast<int>(std::ceil(26.0 / g.cell_width())));
  for (const RefNode& n : layout_.nodes) {
    if (n.mirror || (n.column - first) % stride != 0) {
      continue;
    }
    painter->drawText(QRectF(g.x_of(n.column, first) - 14, height() - g.margin_bottom + 2, 28, 16), Qt::AlignCenter,
                      QString::number(n.poc));
  }
}

void RefGraph::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
  QQuickPaintedItem::geometryChange(new_geometry, old_geometry);
  update();
}

void RefGraph::mousePressEvent(QMouseEvent* event) {
  const int d = frameAt(event->position().x(), event->position().y());
  if (d >= 0 && !model_.isNull()) {
    Q_EMIT frameClicked(d, model_->frames().display_index_of(d));
  }
  event->setAccepted(d >= 0);
}

void RefGraph::hoverMoveEvent(QHoverEvent* event) {
  const int d = frameAt(event->position().x(), event->position().y());
  if (d != hover_frame_) {
    hover_frame_ = d;
    update();
    Q_EMIT frameHovered(d);
  }
}

void RefGraph::hoverLeaveEvent(QHoverEvent* /*event*/) {
  if (hover_frame_ != -1) {
    hover_frame_ = -1;
    update();
    Q_EMIT frameHovered(-1);
  }
}

void RefGraph::wheelEvent(QWheelEvent* event) {
  const double notches = event->angleDelta().y() / 120.0;
  if (notches == 0.0) {
    return;
  }
  scrollWindow(static_cast<int>(std::lround(-notches * kWheelStep)));
  event->accept();
}

}  // namespace ovt::app
