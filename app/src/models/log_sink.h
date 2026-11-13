// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LogSink (chapter 11 §11.3.16, WI-1.453): the ring buffer of 10 000 log lines behind the Log tab. It installs
// itself as the core's logger (ovt_set_logger, exported C API); lines may arrive on any thread, are queued under a
// mutex and moved into the model on the GUI thread in batches every kFlushMs. `filtered` is the view the tab shows
// (level, module and text filters).

#ifndef OVT_APP_MODELS_LOG_SINK_H
#define OVT_APP_MODELS_LOG_SINK_H

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace ovt::app {

struct LogLine {
  qint64 time_ms = 0;  // since the epoch, UTC
  int level = 0;       // OVT_LOG_TRACE .. OVT_LOG_FATAL
  QString module;
  QString message;  // starts with "[CODE] " when the line has a diagnostic code
};

class LogFilterModel : public QSortFilterProxyModel {
  Q_OBJECT
  QML_ANONYMOUS
  Q_PROPERTY(int minLevel READ minLevel WRITE setMinLevel NOTIFY filterChanged)
  Q_PROPERTY(QString module READ module WRITE setModule NOTIFY filterChanged)
  Q_PROPERTY(QString text READ text WRITE setText NOTIFY filterChanged)

 public:
  explicit LogFilterModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}
  [[nodiscard]] int minLevel() const { return min_level_; }
  [[nodiscard]] QString module() const { return module_; }
  [[nodiscard]] QString text() const { return text_; }
  void setMinLevel(int level);
  void setModule(const QString& module);
  void setText(const QString& text);

 Q_SIGNALS:
  void filterChanged();

 protected:
  [[nodiscard]] bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

 private:
  int min_level_ = 0;
  QString module_;
  QString text_;
};

class LogSink : public QAbstractListModel {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(QObject* filtered READ filtered CONSTANT)
  // While paused, lines queue up (up to the ring size) and are not shown; "follow" is the view's business.
  Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)

 public:
  enum Role { TimeRole = Qt::UserRole + 1, LevelRole, ModuleRole, MessageRole, TextRole };
  Q_ENUM(Role)

  static constexpr std::size_t kCapacity = 10000;
  static constexpr int kFlushMs = 100;

  // `install` false leaves the core's logger alone (tests); the model then receives lines through append().
  LogSink(bool install, QObject* parent);
  LogSink() : LogSink(true, nullptr) {}
  ~LogSink() override;
  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;
  LogSink(LogSink&&) = delete;
  LogSink& operator=(LogSink&&) = delete;

  // Thread-safe; the line shows up after the next flush().
  void append(LogLine line);
  // Moves queued lines into the model (GUI thread). The timer calls it unless paused.
  void flush();

  [[nodiscard]] QObject* filtered() { return &filter_; }
  [[nodiscard]] bool paused() const { return paused_; }
  void setPaused(bool paused);

  // Writes the lines the filter shows, in the format of ovt::log::format_line (chapter 7 §7.1.6).
  Q_INVOKABLE bool save(const QString& path);
  Q_INVOKABLE void clear();

  [[nodiscard]] static QString format(const LogLine& line);
  [[nodiscard]] static QString level_name(int level);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

 Q_SIGNALS:
  void pausedChanged();

 private:
  bool installed_;
  bool paused_ = false;
  QTimer timer_;
  LogFilterModel filter_;
  std::deque<LogLine> lines_;
  std::mutex mutex_;
  std::vector<LogLine> pending_;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_LOG_SINK_H
