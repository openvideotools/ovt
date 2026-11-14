// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// RefGraph (chapter 11 §11.3.11; WI-2.484): the Reference / GOP graph of the Reference Graph panel as a painted
// Qt Quick item. It draws the window of ±64 frames around the selected frame as laid out by ref_graph_layout.h
// (rows by temporal id, decode-vs-display or flat; arrows from `declared_refs`), handles click, hover and wheel, and
// answers the DPB strip's questions. It does not know the Selection singleton: it emits signals and the panel QML
// connects them.

#ifndef OVT_APP_ITEMS_REF_GRAPH_H
#define OVT_APP_ITEMS_REF_GRAPH_H

#include <QtQml/qqmlregistration.h>
#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QString>
#include <QVariantList>

#include "models/ref_graph_layout.h"
#include "models/stream_index_model.h"

namespace ovt::app {

class RefGraph : public QQuickPaintedItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(RefGraph)
  Q_PROPERTY(ovt::app::StreamIndexModel* model READ model WRITE set_model NOTIFY modelChanged)
  // 0 Temporal layers, 1 Decode vs display, 2 Flat (`RefRowMode`).
  Q_PROPERTY(int rowMode READ row_mode WRITE set_row_mode NOTIFY layoutChanged)
  // x axis: false decode order, true display order. Ignored by the decode-vs-display mode.
  Q_PROPERTY(bool displayAxis READ display_axis WRITE set_display_axis NOTIFY layoutChanged)
  // The decode index of the frame to outline; the window recentres on it when it leaves the window. -1: none.
  Q_PROPERTY(int selectedFrame READ selected_frame WRITE set_selected_frame NOTIFY selectedFrameChanged)
  Q_PROPERTY(int windowFirst READ window_first NOTIFY layoutChanged)
  Q_PROPERTY(int nodeCount READ node_count NOTIFY layoutChanged)
  Q_PROPERTY(int arrowCount READ arrow_count NOTIFY layoutChanged)
  Q_PROPERTY(int rowCount READ row_count NOTIFY layoutChanged)
  // Label and outline colours, bound to the application style by the panel.
  Q_PROPERTY(QColor textColor READ text_color WRITE set_text_color NOTIFY styleChanged)
  Q_PROPERTY(QColor outlineColor READ outline_color WRITE set_outline_color NOTIFY styleChanged)
  // False when no frame of the window carries `dpb_after` ("DPB: not derivable for this codec/plugin").
  Q_PROPERTY(bool dpbKnown READ dpb_known NOTIFY layoutChanged)

 public:
  explicit RefGraph(QQuickItem* parent = nullptr);
  ~RefGraph() override;

  [[nodiscard]] StreamIndexModel* model() const noexcept { return model_.data(); }
  void set_model(StreamIndexModel* model);
  [[nodiscard]] int row_mode() const noexcept { return static_cast<int>(layout_.params.mode); }
  void set_row_mode(int mode);
  [[nodiscard]] bool display_axis() const noexcept { return display_axis_; }
  void set_display_axis(bool display);
  [[nodiscard]] int selected_frame() const noexcept { return selected_frame_; }
  void set_selected_frame(int decode_index);
  [[nodiscard]] int window_first() const noexcept { return layout_.params.first_column; }
  [[nodiscard]] int node_count() const noexcept { return static_cast<int>(layout_.nodes.size()); }
  [[nodiscard]] int arrow_count() const noexcept { return static_cast<int>(layout_.arrows.size()); }
  [[nodiscard]] int row_count() const noexcept { return layout_.rows; }
  [[nodiscard]] bool dpb_known() const noexcept { return layout_.dpb_known; }
  [[nodiscard]] QColor text_color() const { return text_color_; }
  void set_text_color(const QColor& c);
  [[nodiscard]] QColor outline_color() const { return outline_color_; }
  void set_outline_color(const QColor& c);

  // The layout of the current window (tests and the QML panel read rows and arrows from it).
  [[nodiscard]] const RefGraphLayout& layout() const noexcept { return layout_; }
  // Row and arrows per frame of the window, for the QML tests: [{decodeIndex, row, column, refsUnknown}].
  Q_INVOKABLE [[nodiscard]] QVariantList nodes() const;
  // [{from, to, targetInWindow}]
  Q_INVOKABLE [[nodiscard]] QVariantList arrows() const;
  // The decode index of the node at an item position, -1 for none.
  Q_INVOKABLE [[nodiscard]] int frameAt(double x, double y) const;
  // The item position of a frame's node ({x, y}; empty when the frame is not in the window).
  Q_INVOKABLE [[nodiscard]] QVariantMap nodePosition(int decode_index) const;
  Q_INVOKABLE [[nodiscard]] QString frameTooltip(int decode_index) const;
  // The DPB strip after `decode_index`: [{decodeIndex, label}]; empty when not derivable.
  Q_INVOKABLE [[nodiscard]] QVariantList dpbChips(int decode_index) const;
  Q_INVOKABLE void scrollWindow(int frames);
  Q_INVOKABLE void copyFrameList() const;
  Q_INVOKABLE void copySvg() const;

  void paint(QPainter* painter) override;

 Q_SIGNALS:
  void modelChanged();
  void layoutChanged();
  void styleChanged();
  void selectedFrameChanged();
  // A click on a node: the frame's decode and display indices.
  void frameClicked(int decodeIndex, int displayIndex);
  // The pointer is over this frame (-1: left the graph).
  void frameHovered(int decodeIndex);

 protected:
  void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
  void mousePressEvent(QMouseEvent* event) override;
  void hoverMoveEvent(QHoverEvent* event) override;
  void hoverLeaveEvent(QHoverEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  static constexpr double kHitRadius = 12.0;
  [[nodiscard]] RefAxis axis() const noexcept;
  [[nodiscard]] RefGraphGeometry geometry() const;
  void relayout(bool recentre);
  void on_index_changed();

  QPointer<StreamIndexModel> model_;
  RefGraphLayout layout_;
  bool display_axis_ = true;
  RefRowMode mode_ = RefRowMode::TemporalLayers;
  int first_column_ = 0;
  int selected_frame_ = -1;
  int hover_frame_ = -1;
  QColor text_color_{0xDD, 0xDD, 0xDD};
  QColor outline_color_{0xFF, 0xFF, 0xFF};
};

}  // namespace ovt::app

#endif  // OVT_APP_ITEMS_REF_GRAPH_H
