// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// StreamIndexModel (chapter 11 §11.3.1; WI-1.455a): the Timeline's view of one StreamIndex. It owns the bytes of
// the index, the FrameTable with its LOD series, the x-axis order, and the selected range with its status-line
// statistics; and it hands out the SelectionIndex adapter that SelectionModel needs (WI-1.452, spec-question #219).
//
// The chart item (WI-1.455b) reads columns() while painting; it never touches the FlatBuffer.

#ifndef OVT_APP_MODELS_STREAM_INDEX_MODEL_H
#define OVT_APP_MODELS_STREAM_INDEX_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "models/frame_table.h"
#include "models/selection_model.h"
#include "models/series_aggregator.h"
#include "models/stream_index_adapter.h"

namespace ovt::app {

class StreamIndexModel : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(StreamIndexModel)
  QML_UNCREATABLE("Owned by the application")
  Q_PROPERTY(int frameCount READ frame_count NOTIFY indexChanged)
  Q_PROPERTY(bool displayOrder READ display_order WRITE set_display_order NOTIFY orderChanged)
  // Valid when a range is set: first and last position (inclusive) in the current order.
  Q_PROPERTY(bool hasRange READ has_range NOTIFY rangeChanged)
  Q_PROPERTY(int rangeFirst READ range_first NOTIFY rangeChanged)
  Q_PROPERTY(int rangeLast READ range_last NOTIFY rangeChanged)
  // frames, bytes, seconds, bitsPerSecond, meanQp, firstDecodeIndex, lastDecodeIndex; empty without a range.
  Q_PROPERTY(QVariantMap rangeStats READ range_stats_map NOTIFY rangeChanged)

 public:
  explicit StreamIndexModel(QObject* parent = nullptr);
  ~StreamIndexModel() override;

  // Replaces the index (nullptr clears). Resets the range. The handle keeps the bytes alive for the adapter too.
  void setIndex(std::shared_ptr<const StreamIndexHandle> handle);
  [[nodiscard]] const std::shared_ptr<const StreamIndexHandle>& handle() const noexcept { return handle_; }
  // The lookups SelectionModel::setIndex wants, over the same bytes; nullptr without an index.
  [[nodiscard]] std::shared_ptr<const SelectionIndex> selectionIndex() const;

  [[nodiscard]] FrameTable& frames() noexcept { return table_; }
  [[nodiscard]] const FrameTable& frames() const noexcept { return table_; }

  [[nodiscard]] int frame_count() const noexcept { return static_cast<int>(table_.size()); }
  [[nodiscard]] bool display_order() const noexcept { return table_.order() == FrameOrder::Display; }
  void set_display_order(bool display);

  // The chart columns of the frames [first, last) across `width` pixels for a series.
  [[nodiscard]] std::vector<SeriesColumn> columns(const SeriesRef& series, std::size_t first, std::size_t last,
                                                  std::size_t width);

  // Sets the selected range (positions, inclusive, either order, clamped) or clears it. Emits rangeChanged(first,
  // last) with the normalised positions; clearing emits rangeChanged(-1, -1).
  // Frame lookups for QML (the panel steps through frames and maps Selection back to positions).
  Q_INVOKABLE [[nodiscard]] int decodeIndexAt(int position) const;
  Q_INVOKABLE [[nodiscard]] int displayIndexOf(int decode_index) const;
  Q_INVOKABLE [[nodiscard]] int positionOf(int decode_index) const;

  Q_INVOKABLE void setRange(int first, int last);
  Q_INVOKABLE void clearRange();
  [[nodiscard]] bool has_range() const noexcept { return range_.has_value(); }
  [[nodiscard]] int range_first() const noexcept { return range_ ? static_cast<int>(range_->first) : -1; }
  [[nodiscard]] int range_last() const noexcept { return range_ ? static_cast<int>(range_->second) : -1; }
  [[nodiscard]] std::optional<RangeStats> range_stats() const;
  [[nodiscard]] QVariantMap range_stats_map() const;

 Q_SIGNALS:
  void indexChanged();
  void orderChanged();
  void rangeChanged(int first, int last);

 private:
  std::shared_ptr<const StreamIndexHandle> handle_;
  FrameTable table_;
  std::optional<std::pair<std::size_t, std::size_t>> range_;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_STREAM_INDEX_MODEL_H
