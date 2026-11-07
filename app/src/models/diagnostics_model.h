// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DiagnosticsModel (chapter 11 §11.3.16, WI-1.453): every Diagnostic of the open streams in one table, merged from
// named sources (one per FlatBuffer vector owner: the stream index, the container tree, a metrics run, the GUI
// itself), capped at kCapacity rows with the oldest dropped. A double-click navigates through a NavigationSink.
//
// SPEC-ASSUMPTION(#137): the SelectionModel of WI-1.452 is not merged, so navigation goes through the small
// NavigationSink interface below, which SelectionModel will implement; and the location of an entry is app-side
// (DiagnosticLocation), filled by whoever builds the entry, because the Diagnostic table carries only a BitSpan.
// The FlatBuffers adapter follows when the application links the generated headers.

#ifndef OVT_APP_MODELS_DIAGNOSTICS_MODEL_H
#define OVT_APP_MODELS_DIAGNOSTICS_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QAbstractTableModel>
#include <QByteArray>
#include <QHash>
#include <QPoint>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ovt::app {

// The severities of the Diagnostic table (ovt_model.fbs: Info, Warning, Error, Fatal).
enum class DiagSeverity : int { Info = 0, Warning = 1, Error = 2, Fatal = 3 };

struct DiagnosticLocation {
  enum class Kind : std::uint8_t { None, UnitBits, Box, FrameBlock, FrameProbe, FileOffset };
  Kind kind = Kind::None;
  int unit_index = -1;  // UnitBits
  std::uint64_t bit_offset = 0;
  std::uint32_t bit_length = 0;
  QString box_path;       // Box: "moov/trak[1]/stsz"
  int decode_index = -1;  // FrameBlock, FrameProbe
  int display_index = -1;
  std::uint32_t block_id = 0xFFFFFFFFU;  // FrameBlock
  QPoint probe{-1, -1};                  // FrameProbe (luma samples)
  std::uint64_t file_offset = 0;         // FileOffset

  // "unit #412 bit 102", "box moov/trak[1]/stsz", "frame #1203 block 37", "frame #7 at (12, 34)", "file offset 0x1f4".
  [[nodiscard]] QString text() const;
};

struct DiagnosticEntry {
  DiagSeverity severity = DiagSeverity::Info;
  QString code;  // "SYN-H265-0007"; shown verbatim, never re-numbered
  QString message;
  QString spec_ref;
  DiagnosticLocation location;

  // The code's family, "SYN" for "SYN-H265-0007" (the module filter).
  [[nodiscard]] QString family() const;
};

// Where a double-click goes; SelectionModel implements it. `origin` is the panel id for echo suppression.
class NavigationSink {
 public:
  NavigationSink() = default;
  NavigationSink(const NavigationSink&) = delete;
  NavigationSink& operator=(const NavigationSink&) = delete;
  NavigationSink(NavigationSink&&) = delete;
  NavigationSink& operator=(NavigationSink&&) = delete;
  virtual ~NavigationSink() = default;

  // Sets unit_index and bit_range (Syntax and Hex follow).
  virtual void showUnitBits(int unit_index, std::uint64_t bit_offset, std::uint32_t bit_length,
                            const QString& origin) = 0;
  // Sets the container box (Container and Hex follow).
  virtual void showBox(const QString& path, const QString& origin) = 0;
  // Sets the frame and the block (Picture follows).
  virtual void showFrameBlock(int decode_index, int display_index, std::uint32_t block_id, const QString& origin) = 0;
  // Sets the frame and pins the probe (VQ-DIFF-0001).
  virtual void showFrameProbe(int decode_index, int display_index, QPoint probe, const QString& origin) = 0;
  virtual void showFileOffset(std::uint64_t offset, const QString& origin) = 0;
};

class DiagnosticsModel : public QAbstractTableModel {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(int errorCount READ errorCount NOTIFY countsChanged)
  Q_PROPERTY(int warningCount READ warningCount NOTIFY countsChanged)
  // Entries dropped because of the cap ("N more…").
  Q_PROPERTY(int droppedCount READ droppedCount NOTIFY countsChanged)
  Q_PROPERTY(QObject* filtered READ filtered CONSTANT)

 public:
  enum Column { SeverityColumn, CodeColumn, MessageColumn, LocationColumn, SpecRefColumn, ColumnCount };
  enum Role {
    SeverityRole = Qt::UserRole + 1,
    CodeRole,
    MessageRole,
    LocationRole,
    SpecRefRole,
    ModuleRole,
    NavigableRole,
  };
  Q_ENUM(Role)

  static constexpr std::size_t kCapacity = 10000;
  static constexpr const char* kOrigin = "diagnostics";

  DiagnosticsModel();
  ~DiagnosticsModel() override;
  DiagnosticsModel(const DiagnosticsModel&) = delete;
  DiagnosticsModel& operator=(const DiagnosticsModel&) = delete;
  DiagnosticsModel(DiagnosticsModel&&) = delete;
  DiagnosticsModel& operator=(DiagnosticsModel&&) = delete;

  // Replaces everything `source` contributed (e.g. "f0/index", "f0/container") and appends the new entries.
  void setSource(const QString& source, std::vector<DiagnosticEntry> entries);
  void removeSource(const QString& source);
  // Appends one entry to the source "gui" (GUI-* codes raised by ovt-studio itself).
  Q_INVOKABLE void addGui(int severity, const QString& code, const QString& message);
  Q_INVOKABLE void clear();

  void setNavigationSink(NavigationSink* sink) { sink_ = sink; }
  // Navigates to the location of the entry in `row`; false when it has none or no sink is installed.
  Q_INVOKABLE bool activate(int row);

  [[nodiscard]] int errorCount() const { return errors_; }
  [[nodiscard]] int warningCount() const { return warnings_; }
  [[nodiscard]] int droppedCount() const { return dropped_; }
  [[nodiscard]] QObject* filtered() { return filter_; }
  [[nodiscard]] const DiagnosticEntry& entry(int row) const { return entries_.at(static_cast<std::size_t>(row)).entry; }
  // "ERROR SYN-H265-0007 message (unit #412 bit 102) [H.265 7.3.2.2.1]"
  [[nodiscard]] static QString plain_text(const DiagnosticEntry& entry);
  [[nodiscard]] static QString severity_name(DiagSeverity severity);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

 Q_SIGNALS:
  void countsChanged();

 private:
  struct Row {
    QString source;
    DiagnosticEntry entry;
  };
  void enforce_cap();
  void recount();

  std::deque<Row> entries_;
  NavigationSink* sink_ = nullptr;
  QSortFilterProxyModel* filter_;
  int errors_ = 0;
  int warnings_ = 0;
  int dropped_ = 0;
};

// The table the Errors tab shows: severity, module and text filters; rows of the proxy map back for activate().
class DiagnosticsFilterModel : public QSortFilterProxyModel {
  Q_OBJECT
  QML_ANONYMOUS
  Q_PROPERTY(int minSeverity READ minSeverity WRITE setMinSeverity NOTIFY filterChanged)
  Q_PROPERTY(QString module READ module WRITE setModule NOTIFY filterChanged)
  Q_PROPERTY(QString text READ text WRITE setText NOTIFY filterChanged)

 public:
  explicit DiagnosticsFilterModel(DiagnosticsModel* source);
  [[nodiscard]] int minSeverity() const { return min_severity_; }
  [[nodiscard]] QString module() const { return module_; }
  [[nodiscard]] QString text() const { return text_; }
  void setMinSeverity(int severity);
  void setModule(const QString& module);
  void setText(const QString& text);

  Q_INVOKABLE bool activate(int row);
  Q_INVOKABLE QString rowText(int row) const;
  Q_INVOKABLE QString allText() const;

 Q_SIGNALS:
  void filterChanged();

 protected:
  [[nodiscard]] bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

 private:
  DiagnosticsModel* source_;
  int min_severity_ = 0;
  QString module_;
  QString text_;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_DIAGNOSTICS_MODEL_H
