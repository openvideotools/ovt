// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/jobs_model.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QModelIndex>
#include <QObject>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <utility>

namespace ovt::app {

namespace {

enum Phase : int { kQueued = 0, kRunning = 1, kFinished = 2 };

struct JobDeleter {
  void operator()(ovt_Job* job) const { ovt_job_free(job); }
};

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

QString format_eta(double seconds) {
  const auto total = static_cast<qint64>(std::ceil(seconds));
  return QStringLiteral("%1:%2 left").arg(total / 60, 2, 10, QLatin1Char('0')).arg(total % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

// State shared between the GUI thread and the worker; outlives the entry when a job is detached.
struct JobContext::Shared {
  std::unique_ptr<ovt_Job, JobDeleter> job;
  std::atomic<int> phase{kQueued};
  std::atomic<int> status{OVT_E_INTERNAL};
  std::atomic<std::int64_t> started_ns{0};
  std::mutex mutex;  // guards the reported progress
  bool reported = false;
  std::uint64_t done = 0;
  std::uint64_t total = 0;
  QString stage;
};

bool JobContext::cancelled() const {
  return ovt_job_is_cancelled(job_) != 0;
}

void JobContext::report(std::uint64_t done, std::uint64_t total, const QString& stage) {
  const std::scoped_lock lock(shared_->mutex);
  shared_->reported = true;
  shared_->done = done;
  shared_->total = total;
  shared_->stage = stage;
}

struct JobsModel::Entry {
  quint64 id = 0;
  QString name;
  QString owner;
  std::shared_ptr<JobContext::Shared> shared;
  int phase = kQueued;
  ovt_status status = OVT_OK;
  double progress = -1.0;
  double eta = -1.0;
  std::uint64_t done = 0;
  std::uint64_t total = 0;
  QString stage;
  std::chrono::steady_clock::time_point finished_at;

  [[nodiscard]] bool active() const { return phase != kFinished; }
};

JobsModel::JobsModel() : JobsModel(0, nullptr) {}

JobsModel::JobsModel(int threads, QObject* parent) : QAbstractListModel(parent) {
  pool_.setMaxThreadCount(threads > 0 ? threads : std::clamp(QThread::idealThreadCount(), 2, 16));
  timer_.setInterval(kTickMs);
  QObject::connect(&timer_, &QTimer::timeout, this, &JobsModel::tick);
}

JobsModel::~JobsModel() {
  timer_.stop();
  cancelAll();
  pool_.waitForDone();
}

quint64 JobsModel::submit(const QString& name, JobBody body, const QString& owner) {
  ovt_Job* raw = nullptr;
  auto shared = std::make_shared<JobContext::Shared>();
  const ovt_status created = ovt_job_create(&raw);
  shared->job.reset(raw);
  auto entry = std::make_unique<Entry>();
  entry->id = next_id_++;
  entry->name = name;
  entry->owner = owner;
  entry->shared = shared;
  const quint64 id = entry->id;
  const int row = static_cast<int>(entries_.size());
  beginInsertRows({}, row, row);
  entries_.push_back(std::move(entry));
  endInsertRows();
  if (created != OVT_OK) {  // out of memory: the job fails at once
    shared->status = created;
    shared->phase = kFinished;
  } else {
    pool_.start([shared, body = std::move(body)]() mutable {
      shared->started_ns = now_ns();
      shared->phase = kRunning;
      JobContext ctx(shared->job.get(), shared);
      ovt_status result = OVT_E_INTERNAL;
      try {
        result = body(ctx);
      } catch (const std::bad_alloc&) {
        result = OVT_E_NOMEM;
      } catch (...) {  // a body must not throw; the job fails instead of the program
        result = OVT_E_INTERNAL;
      }
      shared->status = result;
      shared->phase = kFinished;
    });
  }
  if (!timer_.isActive()) {
    timer_.setInterval(kTickMs);
    timer_.start();
  }
  Q_EMIT changed();
  return id;
}

JobsModel::Entry* JobsModel::find(quint64 id) {
  const auto it = std::ranges::find_if(entries_, [id](const auto& e) { return e->id == id; });
  return it == entries_.end() ? nullptr : it->get();
}

int JobsModel::row_of(quint64 id) const {
  const auto it = std::ranges::find_if(entries_, [id](const auto& e) { return e->id == id; });
  return it == entries_.end() ? -1 : static_cast<int>(it - entries_.begin());
}

void JobsModel::cancel(quint64 id) {
  if (Entry* entry = find(id); entry != nullptr && entry->active()) {
    ovt_job_cancel(entry->shared->job.get());
  }
}

void JobsModel::cancelAll() {
  for (const auto& entry : entries_) {
    if (entry->active()) {
      ovt_job_cancel(entry->shared->job.get());
    }
  }
}

void JobsModel::cancelOwner(const QString& owner, std::chrono::milliseconds wait) {
  for (const auto& entry : entries_) {
    if (entry->owner == owner && entry->active()) {
      ovt_job_cancel(entry->shared->job.get());
    }
  }
  QElapsedTimer waited;
  waited.start();
  const auto pending = [&] {
    return std::ranges::any_of(
        entries_, [&](const auto& e) { return e->owner == owner && e->shared->phase.load() != kFinished; });
  };
  while (pending() && waited.elapsed() < wait.count()) {
    QThread::msleep(5);
  }
  tick();
  for (int row = static_cast<int>(entries_.size()) - 1; row >= 0; --row) {
    const auto& entry = entries_[static_cast<std::size_t>(row)];
    if (entry->owner == owner && entry->active()) {  // detach: the worker still holds the shared state
      const QString name = entry->name;
      beginRemoveRows({}, row, row);
      entries_.erase(entries_.begin() + row);
      endRemoveRows();
      Q_EMIT jobDetached(name);
    }
  }
  Q_EMIT changed();
}

void JobsModel::clearFinished() {
  for (int row = static_cast<int>(entries_.size()) - 1; row >= 0; --row) {
    if (!entries_[static_cast<std::size_t>(row)]->active()) {
      beginRemoveRows({}, row, row);
      entries_.erase(entries_.begin() + row);
      endRemoveRows();
    }
  }
  Q_EMIT changed();
}

void JobsModel::update_entry(Entry& entry) {
  auto& shared = *entry.shared;
  const int phase = shared.phase.load();
  if (phase == kFinished && entry.phase != kFinished) {
    entry.phase = kFinished;
    entry.status = static_cast<ovt_status>(shared.status.load());
    entry.finished_at = std::chrono::steady_clock::now();
    if (entry.status == OVT_OK && entry.total > 0) {
      entry.done = entry.total;
    }
    if (entry.status == OVT_OK) {
      entry.progress = 1.0;
    }
    entry.eta = -1.0;
    Q_EMIT jobFinished(entry.id, static_cast<int>(entry.status));
    return;
  }
  if (phase == kQueued) {
    return;
  }
  entry.phase = kRunning;
  bool reported = false;
  {
    const std::scoped_lock lock(shared.mutex);
    reported = shared.reported;
    if (reported) {
      entry.done = shared.done;
      entry.total = shared.total;
      entry.stage = shared.stage;
    }
  }
  if (!reported) {  // a core call reporting through the ovt_Job
    ovt_JobProgress snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    ovt_job_progress(shared.job.get(), &snapshot);
    entry.done = snapshot.done;
    entry.total = snapshot.total;
    entry.stage = QString::fromUtf8(snapshot.stage != nullptr ? snapshot.stage : "");
  }
  entry.progress =
      entry.total > 0 ? std::min(1.0, static_cast<double>(entry.done) / static_cast<double>(entry.total)) : -1.0;
  entry.eta = -1.0;
  if (entry.progress > 0.01) {
    const double elapsed = static_cast<double>(now_ns() - shared.started_ns.load()) / 1e9;
    entry.eta = elapsed * (1.0 - entry.progress) / entry.progress;
  }
}

void JobsModel::tick() {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = *entries_[i];
    if (!entry.active()) {
      continue;
    }
    const int before_phase = entry.phase;
    const std::uint64_t before_done = entry.done;
    const std::uint64_t before_total = entry.total;
    const QString before_stage = entry.stage;
    update_entry(entry);
    if (entry.phase != before_phase || entry.done != before_done || entry.total != before_total ||
        entry.stage != before_stage) {
      const QModelIndex index = this->index(static_cast<int>(i));
      Q_EMIT dataChanged(index, index);
      Q_EMIT changed();
    }
  }
  prune(std::chrono::steady_clock::now());
  const int wanted = activeCount() > 0 ? kTickMs : 1000;
  if (timer_.interval() != wanted) {
    timer_.setInterval(wanted);
  }
  if (entries_.empty()) {
    timer_.stop();
  }
}

void JobsModel::prune(std::chrono::steady_clock::time_point now) {
  bool removed = false;
  for (int row = static_cast<int>(entries_.size()) - 1; row >= 0; --row) {
    const Entry& entry = *entries_[static_cast<std::size_t>(row)];
    if (entry.active()) {
      continue;
    }
    const auto keep = entry.status == OVT_OK || entry.status == OVT_E_CANCELLED ? kKeepDone : kKeepFailed;
    if (now - entry.finished_at >= keep) {
      beginRemoveRows({}, row, row);
      entries_.erase(entries_.begin() + row);
      endRemoveRows();
      removed = true;
    }
  }
  if (removed) {
    Q_EMIT changed();
  }
}

int JobsModel::activeCount() const {
  return static_cast<int>(std::ranges::count_if(entries_, [](const auto& e) { return e->active(); }));
}

bool JobsModel::foregroundActive() const {
  return activeCount() > 0;
}

namespace {
template <class Entries>
const typename Entries::value_type::element_type* foreground_of(const Entries& entries) {
  const typename Entries::value_type::element_type* queued = nullptr;
  for (const auto& entry : entries) {
    if (entry->phase == kRunning) {
      return entry.get();
    }
    if (entry->phase == kQueued && queued == nullptr) {
      queued = entry.get();
    }
  }
  return queued;
}
}  // namespace

double JobsModel::foregroundProgress() const {
  const Entry* entry = foreground_of(entries_);
  return entry != nullptr ? entry->progress : -1.0;
}

QString JobsModel::foregroundText() const {
  const Entry* entry = foreground_of(entries_);
  return entry != nullptr ? text_of(*entry) : QString();
}

QString JobsModel::text_of(const Entry& entry) {
  switch (entry.phase) {
    case kQueued:
      return QStringLiteral("%1 · queued").arg(entry.name);
    case kFinished:
      if (entry.status == OVT_OK) {
        return QStringLiteral("%1 · done").arg(entry.name);
      }
      return entry.status == OVT_E_CANCELLED ? QStringLiteral("%1 · cancelled").arg(entry.name)
                                             : QStringLiteral("%1 · failed (%2)")
                                                   .arg(entry.name, QString::fromUtf8(ovt_status_string(entry.status)));
    default:
      break;
  }
  // "Indexing… 42 % · 1,203 / 2,870 frames · 00:12 left" (§11.7); the unit word comes with the stage.
  QString text = QStringLiteral("%1…").arg(entry.name);
  if (entry.progress >= 0.0) {
    text += QStringLiteral(" %1 %").arg(static_cast<int>(entry.progress * 100.0));
  }
  if (entry.total > 0) {
    text += QStringLiteral(" · %1 / %2").arg(entry.done).arg(entry.total);
    if (!entry.stage.isEmpty()) {
      text += QLatin1Char(' ') + entry.stage;
    }
  } else if (!entry.stage.isEmpty()) {
    text += QStringLiteral(" · ") + entry.stage;
  }
  if (entry.eta >= 0.0) {
    text += QStringLiteral(" · ") + format_eta(entry.eta);
  }
  return text;
}

int JobsModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant JobsModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(entries_.size())) {
    return {};
  }
  const Entry& entry = *entries_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case IdRole:
      return entry.id;
    case Qt::DisplayRole:
    case NameRole:
      return entry.name;
    case StateRole:
      if (entry.phase == kQueued) {
        return QStringLiteral("queued");
      }
      if (entry.phase == kRunning) {
        return QStringLiteral("running");
      }
      return entry.status == OVT_OK
                 ? QStringLiteral("done")
                 : (entry.status == OVT_E_CANCELLED ? QStringLiteral("cancelled") : QStringLiteral("failed"));
    case ProgressRole:
      return entry.progress;
    case TextRole:
      return text_of(entry);
    case EtaRole:
      return entry.eta;
    case ErrorRole:
      return entry.phase == kFinished && entry.status != OVT_OK && entry.status != OVT_E_CANCELLED
                 ? QString::fromUtf8(ovt_status_string(entry.status))
                 : QString();
    case FinishedRole:
      return entry.phase == kFinished;
    default:
      return {};
  }
}

QHash<int, QByteArray> JobsModel::roleNames() const {
  return {{IdRole, "jobId"},  {NameRole, "name"}, {StateRole, "state"}, {ProgressRole, "progress"},
          {TextRole, "text"}, {EtaRole, "eta"},   {ErrorRole, "error"}, {FinishedRole, "finished"}};
}

}  // namespace ovt::app
