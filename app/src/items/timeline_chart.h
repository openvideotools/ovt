// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TimelineChart (chapter 11 §11.3.1; WI-1.455b): the per-frame bar chart of the Timeline panel as a QQuickItem with
// scene-graph nodes (one for the bars, one for the mean line, one for the range band and the selected frame). It
// paints from the LOD columns of the StreamIndexModel, never from the FlatBuffer, and handles click, drag, wheel,
// double-click, hover and the arrow keys. It does not know the Selection singleton: it emits signals and the panel
// QML connects them.

#ifndef OVT_APP_ITEMS_TIMELINE_CHART_H
#define OVT_APP_ITEMS_TIMELINE_CHART_H

#include <QtQml/qqmlregistration.h>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QVariantList>

#include <array>
#include <vector>

#include "models/series_aggregator.h"
#include "models/stream_index_model.h"
#include "models/timeline_geometry.h"
#include "models/timeline_viewport.h"

namespace ovt::app {

class TimelineChart : public QQuickItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(TimelineChart)
  Q_PROPERTY(ovt::app::StreamIndexModel* model READ model WRITE set_model NOTIFY modelChanged)
  // 0 Bits, 1 Qp, 2 TemporalId, 3 Metric (the `Series` enum of frame_table.h).
  Q_PROPERTY(int series READ series WRITE set_series NOTIFY seriesChanged)
  Q_PROPERTY(QString metricId READ metric_id WRITE set_metric_id NOTIFY seriesChanged)
  Q_PROPERTY(int plane READ plane WRITE set_plane NOTIFY seriesChanged)
  // Draw the mean of each pixel column as a line over the bars.
  Q_PROPERTY(bool meanLine READ mean_line WRITE set_mean_line NOTIFY meanLineChanged)
  // The decode index of the frame to mark (the panel binds it to Selection.frameDecodeIndex); -1 none.
  Q_PROPERTY(int selectedFrame READ selected_frame WRITE set_selected_frame NOTIFY selectedFrameChanged)
  // Reference arrows (arcs from each frame to its declared_refs) in the current order; drawn only when a frame is at
  // least kMinArrowFramePixels wide.
  Q_PROPERTY(bool refArrows READ ref_arrows WRITE set_ref_arrows NOTIFY refArrowsChanged)
  // The random access points in view as [{x, label}] ("K" for a key frame, else "I"), for the label overlay.
  Q_PROPERTY(QVariantList gopMarkers READ gop_markers NOTIFY viewChanged)
  Q_PROPERTY(double viewFirst READ view_first NOTIFY viewChanged)
  Q_PROPERTY(double viewCount READ view_count NOTIFY viewChanged)
  // Top of the y axis (the largest bar value in view), for the axis labels.
  Q_PROPERTY(double maxValue READ max_value NOTIFY viewChanged)

 public:
  explicit TimelineChart(QQuickItem* parent = nullptr);
  ~TimelineChart() override;

  [[nodiscard]] StreamIndexModel* model() const noexcept { return model_.data(); }
  void set_model(StreamIndexModel* model);
  [[nodiscard]] int series() const noexcept { return static_cast<int>(series_.kind); }
  void set_series(int series);
  [[nodiscard]] QString metric_id() const { return QString::fromStdString(series_.metric_id); }
  void set_metric_id(const QString& id);
  [[nodiscard]] int plane() const noexcept { return series_.plane; }
  void set_plane(int plane);
  [[nodiscard]] bool mean_line() const noexcept { return mean_line_; }
  void set_mean_line(bool on);
  [[nodiscard]] bool ref_arrows() const noexcept { return ref_arrows_; }
  void set_ref_arrows(bool on);
  [[nodiscard]] QVariantList gop_markers() const;
  [[nodiscard]] int selected_frame() const noexcept { return selected_frame_; }
  void set_selected_frame(int decode_index);
  [[nodiscard]] double view_first() const noexcept { return view_.first(); }
  [[nodiscard]] double view_count() const noexcept { return view_.count(); }
  [[nodiscard]] double max_value() const noexcept { return max_value_; }

  // The tooltip of the frame at a position of the current order:
  // "#disp/#dec · type · bytes · QP · Tid · POC · pts"; empty for an invalid position.
  Q_INVOKABLE [[nodiscard]] QString frameTooltip(int position) const;
  // The frame position (current order) under an x in item coordinates.
  Q_INVOKABLE [[nodiscard]] int positionAt(double x) const;
  // The x (item coordinates) of the middle of a frame, or -1 when it is not in the visible range (bookmark markers).
  Q_INVOKABLE [[nodiscard]] double xOfFrame(int decode_index) const;
  Q_INVOKABLE void fit();
  Q_INVOKABLE void zoomAbout(double notches, double x);
  Q_INVOKABLE void pan(double frames);

 Q_SIGNALS:
  void modelChanged();
  void seriesChanged();
  void meanLineChanged();
  void refArrowsChanged();
  void selectedFrameChanged();
  void viewChanged();
  // A click on a bar: the frame's decode and display indices.
  void frameClicked(int decodeIndex, int displayIndex);
  // The arrow keys: -1 / +1 positions in the current order.
  void stepRequested(int delta);
  // The pointer is over this position (-1: left the chart); x in item coordinates.
  void hovered(int position, double x);

 protected:
  QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
  void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void hoverMoveEvent(QHoverEvent* event) override;
  void hoverLeaveEvent(QHoverEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  // Arrows are drawn when a frame is at least this many device pixels wide.
  static constexpr double kMinArrowFramePixels = 6.0;
  // One vertex list per scene-graph layer, bottom to top: band, bars, hatch, mean line, arrows, ticks, marker.
  static constexpr int LayerCount = 7;
  using LayerVertices = std::array<std::vector<ColorVertex>, LayerCount>;
  LayerVertices build_layers(float w, float h);
  [[nodiscard]] float x_of_position(double position, float w) const;
  [[nodiscard]] std::vector<ColorVertex> hatch_vertices(const std::vector<SeriesColumn>& columns, float w,
                                                        float h) const;
  [[nodiscard]] std::vector<ColorVertex> arc_vertices(float w, float h) const;
  void on_index_changed();
  void view_changed();

  QPointer<StreamIndexModel> model_;
  SeriesRef series_;
  bool mean_line_ = false;
  bool ref_arrows_ = false;
  int hover_position_ = -1;  // position (current order) under the pointer, for the arrow highlight
  int selected_frame_ = -1;
  TimelineViewport view_;
  double max_value_ = 0.0;
  // Pointer interaction: a press that moves less than kDragThreshold px is a click, otherwise a range drag.
  static constexpr double kDragThreshold = 3.0;
  bool pressed_ = false;
  bool dragging_ = false;
  double press_x_ = 0.0;
  int press_position_ = 0;
};

}  // namespace ovt::app

#endif  // OVT_APP_ITEMS_TIMELINE_CHART_H
