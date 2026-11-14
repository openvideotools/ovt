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
  return {c.r, c.g, c.b, c.a};
}

// Everything the paint helpers share.
struct PaintContext {
  QPainter* painter;
  const RefGraphLayout& layout;
  RefGraphGeometry g;
  int first;
  double size;
  QColor text;
  QColor outline;
  int hover;
  int selected;
  double item_height;
  [[nodiscard]] QPointF centre(const RefNode& n) const { return {g.x_of(n.column, first), g.y_of(n.row)}; }
};

// Row labels (T0, T1, ... or Decode / Display).
void draw_row_labels(const PaintContext& c) {
  c.painter->setPen(c.text);
  for (int r = 0; r < c.layout.rows; ++r) {
    QString label;
    if (c.layout.params.mode == RefRowMode::TemporalLayers) {
      label = QStringLiteral("T%1").arg(r);
    } else if (c.layout.params.mode == RefRowMode::DecodeVsDisplay) {
      label = r == 0 ? QObject::tr("Dec") : QObject::tr("Disp");
    }
    c.painter->drawText(QRectF(2, c.g.y_of(r) - 8, c.g.margin_left - 4, 16), Qt::AlignVCenter | Qt::AlignLeft, label);
  }
}

// Decode-vs-display links between a frame's two nodes.
void draw_links(const PaintContext& c) {
  if (c.layout.params.mode != RefRowMode::DecodeVsDisplay) {
    return;
  }
  c.painter->setPen(QPen(QColor(c.outline.red(), c.outline.green(), c.outline.blue(), 70), 1));
  for (const RefNode& n : c.layout.nodes) {
    const RefNode* primary = n.mirror ? c.layout.find(n.decode_index) : nullptr;
    if (primary != nullptr) {
      c.painter->drawLine(c.centre(*primary), c.centre(n));
    }
  }
}

void draw_arrows(const PaintContext& c) {
  const bool dim = c.hover >= 0;
  for (const RefArrow& a : c.layout.arrows) {
    const RefNode* from = c.layout.find(a.from_decode);
    const RefNode* to = c.layout.find(a.to_decode);
    if (from == nullptr || to == nullptr) {
      continue;
    }
    const bool hot = a.from_decode == c.hover || a.to_decode == c.hover;
    QColor color(hot ? kArrowHoverColor : kArrowColor);
    color.setAlpha(hot || !dim ? 230 : 90);
    c.painter->setPen(QPen(color, hot ? 2 : 1));
    const QPointF p0 = c.centre(*from);
    const QPointF p1 = c.centre(*to);
    const double lift = std::min(std::abs(p1.x() - p0.x()) * 0.35 + 6.0, c.g.row_height());
    QPainterPath path(p0);
    path.quadTo(QPointF((p0.x() + p1.x()) / 2, std::min(p0.y(), p1.y()) - lift), p1);
    c.painter->setBrush(Qt::NoBrush);
    c.painter->drawPath(path);
  }
}

// Nodes: filled by frame type; an open node has no known references; a hidden frame is drawn at half opacity.
void draw_nodes(const PaintContext& c) {
  QFont small = c.painter->font();
  small.setPixelSize(std::max(8, static_cast<int>(c.size * 0.6)));
  c.painter->setFont(small);
  for (const RefNode& n : c.layout.nodes) {
    const QPointF centre = c.centre(n);
    const QRectF box(centre.x() - c.size / 2, centre.y() - c.size / 2, c.size, c.size);
    const QColor type_color = to_qcolor(frame_type_color(n.type));
    c.painter->setOpacity(n.shown ? 1.0 : 0.55);
    if (n.refs_unknown) {
      c.painter->setPen(QPen(type_color, 1.5, Qt::DashLine));
      c.painter->setBrush(Qt::NoBrush);
    } else {
      c.painter->setPen(Qt::NoPen);
      c.painter->setBrush(type_color);
    }
    c.painter->drawRoundedRect(box, 2, 2);
    c.painter->setOpacity(1.0);
    if (c.size >= 11) {
      c.painter->setPen(n.refs_unknown ? c.text : QColor(Qt::white));
      c.painter->drawText(box, Qt::AlignCenter, QString::fromLatin1(frame_type_label(n.type)));
    }
    if (n.decode_index == c.selected) {
      c.painter->setPen(QPen(c.outline, 2));
      c.painter->setBrush(Qt::NoBrush);
      c.painter->drawRoundedRect(box.adjusted(-2, -2, 2, 2), 3, 3);
    }
  }
}

// Column labels: POC every few columns that fit.
void draw_column_labels(const PaintContext& c) {
  c.painter->setPen(c.text);
  const int stride = std::max(1, static_cast<int>(std::ceil(26.0 / c.g.cell_width())));
  for (const RefNode& n : c.layout.nodes) {
    if (n.mirror || (n.column - c.first) % stride != 0) {
      continue;
    }
    c.painter->drawText(QRectF(c.g.x_of(n.column, c.first) - 14, c.item_height - c.g.margin_bottom + 2, 28, 16),
                        Qt::AlignCenter, QString::number(n.poc));
  }
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
  PaintContext ctx{
      painter,      layout_,         geometry(), layout_.params.first_column, 0.0, text_color_, outline_color_,
      hover_frame_, selected_frame_, height()};
  ctx.size = std::min({ctx.g.cell_width() * 0.8, ctx.g.row_height() * 0.8, 18.0});
  draw_row_labels(ctx);
  draw_links(ctx);
  draw_arrows(ctx);
  draw_nodes(ctx);
  draw_column_labels(ctx);
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
