// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/log_sink.h"

#include <QDateTime>
#include <QFile>
#include <QModelIndex>
#include <QObject>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <iterator>
#include <new>
#include <utility>

#include "ovt/log.h"

namespace ovt::app {

namespace {

// ovt_log_fn: any thread, concurrently, must not throw or call back into OVT (chapter 12 §12.3.1 rules 4 and 8).
void library_log(int level, const char* module, const char* msg, void* user) {
  try {
    LogLine line;
    line.time_ms = QDateTime::currentMSecsSinceEpoch();
    line.level = level;
    line.module = QString::fromUtf8(module != nullptr ? module : "");
    line.message = QString::fromUtf8(msg != nullptr ? msg : "");
    static_cast<LogSink*>(user)->append(std::move(line));
  } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch): a dropped log line is not worth more
  }
}

}  // namespace

void LogFilterModel::setMinLevel(int level) {
  min_level_ = level;
  invalidateFilter();
  Q_EMIT filterChanged();
}

void LogFilterModel::setModule(const QString& module) {
  module_ = module;
  invalidateFilter();
  Q_EMIT filterChanged();
}

void LogFilterModel::setText(const QString& text) {
  text_ = text;
  invalidateFilter();
  Q_EMIT filterChanged();
}

bool LogFilterModel::filterAcceptsRow(int row, const QModelIndex& parent) const {
  const QModelIndex index = sourceModel()->index(row, 0, parent);
  if (sourceModel()->data(index, LogSink::LevelRole).toInt() < min_level_) {
    return false;
  }
  if (!module_.isEmpty() && sourceModel()->data(index, LogSink::ModuleRole).toString() != module_) {
    return false;
  }
  return text_.isEmpty() ||
         sourceModel()->data(index, LogSink::MessageRole).toString().contains(text_, Qt::CaseInsensitive);
}

LogSink::LogSink(bool install, QObject* parent) : QAbstractListModel(parent), installed_(install), filter_(this) {
  filter_.setSourceModel(this);
  timer_.setInterval(kFlushMs);
  QObject::connect(&timer_, &QTimer::timeout, this, &LogSink::flush);
  timer_.start();
  if (installed_) {
    ovt_set_logger(&library_log, this);
  }
}

LogSink::~LogSink() {
  // ovt_set_logger (ovt/log.h) does not wait for a line already being delivered on another thread, so the
  // sink must outlive any thread that can still log: the application owns it for its whole lifetime.
  if (installed_) {
    ovt_set_logger(nullptr, nullptr);
  }
}

void LogSink::append(LogLine line) {
  const std::scoped_lock lock(mutex_);
  if (pending_.size() >= kCapacity) {  // paused for long: the oldest queued line goes, as in the ring
    pending_.erase(pending_.begin());
  }
  pending_.push_back(std::move(line));
}

void LogSink::flush() {
  std::vector<LogLine> batch;
  {
    const std::scoped_lock lock(mutex_);
    batch.swap(pending_);
  }
  if (batch.empty()) {
    return;
  }
  if (batch.size() > kCapacity) {
    batch.erase(batch.begin(), batch.end() - static_cast<std::ptrdiff_t>(kCapacity));
  }
  const std::size_t overflow = lines_.size() + batch.size() > kCapacity ? lines_.size() + batch.size() - kCapacity : 0;
  if (overflow > 0) {
    beginRemoveRows({}, 0, static_cast<int>(overflow) - 1);
    lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(overflow));
    endRemoveRows();
  }
  const int first = static_cast<int>(lines_.size());
  beginInsertRows({}, first, first + static_cast<int>(batch.size()) - 1);
  std::ranges::move(batch, std::back_inserter(lines_));
  endInsertRows();
}

void LogSink::setPaused(bool paused) {
  if (paused_ == paused) {
    return;
  }
  paused_ = paused;
  if (paused_) {
    timer_.stop();
  } else {
    timer_.start();
    flush();
  }
  Q_EMIT pausedChanged();
}

void LogSink::clear() {
  beginResetModel();
  lines_.clear();
  endResetModel();
}

QString LogSink::level_name(int level) {
  static const std::array<const char*, 7> names{"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL", "OFF  "};
  return QString::fromLatin1(names.at(static_cast<std::size_t>(std::clamp(level, 0, 6))));
}

QString LogSink::format(const LogLine& line) {
  const QString time = QDateTime::fromMSecsSinceEpoch(line.time_ms).toUTC().toString(Qt::ISODateWithMs);
  return QStringLiteral("%1 %2 %3 %4").arg(time, level_name(line.level), line.module, line.message);
}

bool LogSink::save(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    return false;
  }
  QTextStream out(&file);
  for (int row = 0; row < filter_.rowCount(); ++row) {
    out << filter_.data(filter_.index(row, 0), TextRole).toString() << '\n';
  }
  out.flush();
  return out.status() == QTextStream::Ok && file.error() == QFileDevice::NoError;
}

int LogSink::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(lines_.size());
}

QVariant LogSink::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(lines_.size())) {
    return {};
  }
  const LogLine& line = lines_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case TimeRole:
      return line.time_ms;
    case LevelRole:
      return line.level;
    case ModuleRole:
      return line.module;
    case MessageRole:
      return line.message;
    case Qt::DisplayRole:
    case TextRole:
      return format(line);
    default:
      return {};
  }
}

QHash<int, QByteArray> LogSink::roleNames() const {
  return {
      {TimeRole, "time"}, {LevelRole, "level"}, {ModuleRole, "module"}, {MessageRole, "message"}, {TextRole, "text"}};
}

}  // namespace ovt::app
