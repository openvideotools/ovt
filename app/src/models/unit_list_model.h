// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// UnitListModel (chapter 11 §11.3.6; WI-1.459): the Unit list panel's table of NAL units / OBUs / raw samples in
// decode order, as a filtering and sorting proxy over the StreamIndex held by StreamIndexModel.
//
// Rows are an index permutation over StreamIndex.units: the filter keeps the matching unit indices, a stable sort
// reorders that vector, and cells are read from the FlatBuffer in place (10^6 units need no per-row copies).
// Columns: # · Type · L/T · Offset · Size · Frame · Sample · Flags. Click selects the unit; double-click on a VCL
// unit seeks to its frame (Selection.setFrame). Unit types are shown with their spec names (type_name of the index).
//
// SPEC-ASSUMPTION(#240): the "Errors only" chip and the ⚠ count use Unit.diagnostics (the diagnostics raised while
// parsing that unit); stream-level diagnostics are not attributed to units.

#ifndef OVT_APP_MODELS_UNIT_LIST_MODEL_H
#define OVT_APP_MODELS_UNIT_LIST_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QAbstractTableModel>
#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <vector>

#include "models/selection_model.h"
#include "models/stream_index_adapter.h"
#include "models/stream_index_model.h"

namespace ovt::app {

// How the Type column is tinted (§11.3.6): VCL grey, parameter set blue, SEI/metadata green, AUD/filler light.
enum class UnitClass : std::uint8_t { Other, Vcl, ParameterSet, Sei, Aud };

// The class of a unit of `codec` with the given type that does / does not own a frame. A unit that owns a frame is
// always VCL; otherwise the type decides (nal_unit_type for H.264 and H.265, obu_type for AV1).
[[nodiscard]] UnitClass classify_unit(fb::Codec codec, int type, bool has_frame);

class UnitListModel : public QAbstractTableModel {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(StreamIndexModel* streamIndex READ stream_index WRITE set_stream_index NOTIFY streamIndexChanged)
  Q_PROPERTY(SelectionModel* selection READ selection WRITE set_selection NOTIFY selectionChanged)
  // 0 All, 1 VCL, 2 non-VCL, 3 SEI, 4 errors only (units with diagnostics).
  Q_PROPERTY(int kind READ kind WRITE set_kind NOTIFY filterChanged)
  // Case-insensitive substring of the type name.
  Q_PROPERTY(QString text READ text WRITE set_text NOTIFY filterChanged)
  // Offset and Size as 0x hex (the ⓗ toggle of §11.3.6) instead of decimal.
  Q_PROPERTY(bool hexNumbers READ hex_numbers WRITE set_hex_numbers NOTIFY hexNumbersChanged)
  Q_PROPERTY(int unitCount READ unit_count NOTIFY streamIndexChanged)
  Q_PROPERTY(int sortColumn READ sort_column NOTIFY sortChanged)
  Q_PROPERTY(bool sortDescending READ sort_descending NOTIFY sortChanged)

 public:
  enum Column : int {
    ColNumber = 0,
    ColType,
    ColLayerTemporal,
    ColOffset,
    ColSize,
    ColFrame,
    ColSample,
    ColFlags,
    ColumnCount
  };
  Q_ENUM(Column)

  enum Kind : int { KindAll = 0, KindVcl, KindNonVcl, KindSei, KindErrors };
  Q_ENUM(Kind)

  enum Role : int {
    ClassRole = Qt::UserRole + 1,  // "vcl" | "parameterSet" | "sei" | "aud" | "other"
    UnitIndexRole,                 // the StreamIndex unit index of the row
    DiagnosticCountRole,
    IsVclRole,
  };
  Q_ENUM(Role)

  explicit UnitListModel(QObject* parent = nullptr);
  ~UnitListModel() override;
  UnitListModel(const UnitListModel&) = delete;
  UnitListModel& operator=(const UnitListModel&) = delete;
  UnitListModel(UnitListModel&&) = delete;
  UnitListModel& operator=(UnitListModel&&) = delete;

  [[nodiscard]] StreamIndexModel* stream_index() const { return stream_index_; }
  void set_stream_index(StreamIndexModel* model);
  [[nodiscard]] SelectionModel* selection() const { return selection_; }
  void set_selection(SelectionModel* selection);
  // Takes the index directly (tests, and callers without a StreamIndexModel). nullptr clears.
  void setHandle(std::shared_ptr<const StreamIndexHandle> handle);

  [[nodiscard]] int kind() const noexcept { return kind_; }
  void set_kind(int kind);
  [[nodiscard]] QString text() const { return text_; }
  void set_text(const QString& text);
  [[nodiscard]] bool hex_numbers() const noexcept { return hex_numbers_; }
  void set_hex_numbers(bool hex);
  [[nodiscard]] int unit_count() const noexcept;
  [[nodiscard]] int sort_column() const noexcept { return sort_column_; }
  [[nodiscard]] bool sort_descending() const noexcept { return sort_descending_; }

  // QAbstractTableModel
  [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                    int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
  // Stable sort by a column (the order of equal rows is the decode order, or the previous order when descending).
  void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

  // The StreamIndex unit index of a row, or -1.
  Q_INVOKABLE [[nodiscard]] int unitAt(int row) const;
  // The row showing a unit, or -1 when it is filtered out or does not exist.
  Q_INVOKABLE [[nodiscard]] int rowOfUnit(int unit_index) const;
  // Click: Selection.setUnit (which also selects the frame of a VCL unit).
  Q_INVOKABLE void select(int row, const QString& origin);
  // Double-click: a VCL unit seeks to its frame (Selection.setFrame); another unit is only selected.
  Q_INVOKABLE void activate(int row, const QString& origin);
  Q_INVOKABLE void toggleSort(int column);
  // The tab-separated cells of a row ("Copy row").
  Q_INVOKABLE [[nodiscard]] QString rowText(int row) const;

 Q_SIGNALS:
  void streamIndexChanged();
  void selectionChanged();
  void filterChanged();
  void hexNumbersChanged();
  void sortChanged();

 private:
  [[nodiscard]] const fb::Unit* unit_ptr(std::uint32_t unit_index) const;
  [[nodiscard]] UnitClass class_of(std::uint32_t unit_index) const;
  [[nodiscard]] QString cell(std::uint32_t unit_index, int column) const;
  [[nodiscard]] static QString type_label(const fb::Unit& unit);
  [[nodiscard]] bool matches(std::uint32_t unit_index) const;
  // Rebuilds rows_ (filter, then sort) inside a model reset.
  void rebuild();
  void sort_rows();
  [[nodiscard]] bool less(std::uint32_t a, std::uint32_t b) const;
  void on_index_changed();

  StreamIndexModel* stream_index_ = nullptr;  // not owned; cleared when it is destroyed
  SelectionModel* selection_ = nullptr;       // not owned; cleared when it is destroyed
  std::shared_ptr<const StreamIndexHandle> handle_;
  std::vector<std::uint8_t> classes_;  // UnitClass per unit, from the codec and type
  std::vector<std::uint32_t> rows_;    // unit indices in display order
  int kind_ = KindAll;
  QString text_;
  bool hex_numbers_ = false;
  int sort_column_ = ColNumber;
  bool sort_descending_ = false;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_UNIT_LIST_MODEL_H
