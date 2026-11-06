// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// JobsModel (chapter 11 §11.3.18, §11.7; WI-1.453): the list of running, queued and finished jobs behind the status
// bar, the toolbar indicator and the Jobs popover. Every core call longer than 50 ms runs as a job: the model owns an
// ovt_Job (cancellation and progress, ovt/jobs.h) per job and runs the body on a worker thread of its own.
//
// SPEC-ASSUMPTION(#137): the step text says "on ovt::jobs::Pool", but Pool is a C++ helper of ovt_core_objects
// that the shared library does not export (hidden visibility, chapter 12 §12.3.2), so the GUI uses the exported C
// ovt_Job API and a QThreadPool. Core calls take the ovt_Job and report their own progress; a body that has no core
// call to make reports through JobContext::report().
//
// Everything here runs on the GUI thread except the job bodies. Progress is polled from the GUI thread every
// kTickMs while a job is active, so the status bar lags a report by at most one tick (acceptance test 1: 100 ms).

#ifndef OVT_APP_MODELS_JOBS_MODEL_H
#define OVT_APP_MODELS_JOBS_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVariant>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "ovt/jobs.h"
#include "ovt/status.h"

namespace ovt::app {

// What a job body sees. Valid only during the body.
class JobContext {
 public:
  // The handle to pass to core calls that take an ovt_Job (they report progress and check cancellation).
  [[nodiscard]] ovt_Job* handle() const { return job_; }
  [[nodiscard]] bool cancelled() const;
  // For bodies that do not call the core: total = 0 is indeterminate. Thread-safe.
  void report(std::uint64_t done, std::uint64_t total, const QString& stage = QString());

 private:
  friend class JobsModel;
  struct Shared;
  JobContext(ovt_Job* job, std::shared_ptr<Shared> shared) : job_(job), shared_(std::move(shared)) {}
  ovt_Job* job_;
  std::shared_ptr<Shared> shared_;
};

using JobBody = std::function<ovt_status(JobContext&)>;

class JobsModel : public QAbstractListModel {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  // Queued or running jobs, for the toolbar indicator.
  Q_PROPERTY(int activeCount READ activeCount NOTIFY changed)
  // The foreground job (the oldest active one) for the status bar: progress 0..1, or -1 when indeterminate.
  Q_PROPERTY(bool foregroundActive READ foregroundActive NOTIFY changed)
  Q_PROPERTY(double foregroundProgress READ foregroundProgress NOTIFY changed)
  Q_PROPERTY(QString foregroundText READ foregroundText NOTIFY changed)

 public:
  enum Role {
    IdRole = Qt::UserRole + 1,
    NameRole,
    StateRole,     // "queued", "running", "done", "failed", "cancelled"
    ProgressRole,  // 0..1 or -1
    TextRole,      // the progress wording of §11.7
    EtaRole,       // seconds left, -1 when unknown
    ErrorRole,     // "OVT_E_IO" etc. for a failed job, else empty
    FinishedRole,
  };
  Q_ENUM(Role)

  static constexpr int kTickMs = 50;                             // < 100 ms (acceptance test 1)
  static constexpr std::chrono::seconds kKeepFailed{5 * 60};     // §11.3.18
  static constexpr std::chrono::seconds kKeepDone{5};            // a finished job lingers briefly
  static constexpr std::chrono::milliseconds kCancelWait{2000};  // §11.7 close-file cancellation

  JobsModel();
  explicit JobsModel(int threads, QObject* parent = nullptr);
  ~JobsModel() override;
  JobsModel(const JobsModel&) = delete;
  JobsModel& operator=(const JobsModel&) = delete;
  JobsModel(JobsModel&&) = delete;
  JobsModel& operator=(JobsModel&&) = delete;

  // Queues `body`. `owner` names the file the job belongs to ("" = none) for cancelOwner(). Returns the job id.
  quint64 submit(const QString& name, JobBody body, const QString& owner = QString());

  Q_INVOKABLE void cancel(quint64 id);
  Q_INVOKABLE void cancelAll();
  // Close-file cancellation (§11.7): cancels the owner's jobs and waits up to kCancelWait for them; the ones that
  // did not stop are detached (dropped from the list, their results ignored) and reported with jobDetached().
  void cancelOwner(const QString& owner, std::chrono::milliseconds wait = kCancelWait);
  Q_INVOKABLE void clearFinished();

  // Reads progress, moves finished jobs to their final state and drops the expired ones. The timer calls it.
  void tick();
  void prune(std::chrono::steady_clock::time_point now);

  [[nodiscard]] int activeCount() const;
  [[nodiscard]] bool foregroundActive() const;
  [[nodiscard]] double foregroundProgress() const;
  [[nodiscard]] QString foregroundText() const;

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

 Q_SIGNALS:
  void changed();
  void jobFinished(quint64 id, int status);
  // GUI-JOB-0001: the job did not stop within the wait and runs on in the background.
  void jobDetached(const QString& name);

 private:
  struct Entry;
  Entry* find(quint64 id);
  [[nodiscard]] int row_of(quint64 id) const;
  void update_entry(Entry& entry);
  [[nodiscard]] static QString text_of(const Entry& entry);

  QThreadPool pool_;
  QTimer timer_;
  std::vector<std::unique_ptr<Entry>> entries_;
  quint64 next_id_ = 1;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_JOBS_MODEL_H
