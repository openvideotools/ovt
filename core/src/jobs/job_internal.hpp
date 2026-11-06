// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Internal side of the jobs module (WI-1.512): the ovt_Job object behind ovt/jobs.h and the helper every
// blocking C entry point uses to run its work under an ovt_Job (chapter 12 §12.3.5). Not installed.
//
// Usage in a later C entry point (WI-1.121, WI-1.204, decode loops; this WI inserts nothing elsewhere):
//
//   OVT_API ovt_status ovt_stream_build_index(ovt_Stream* s, ovt_Job* job) noexcept {
//     return ovt::jobs::run(job, "index", [&](ovt::jobs::Cancel& cancel, const auto& report) {
//       ...  // every <= 50 ms of work: cancel.throw_if_requested(); report({done, total, "index"});
//       return OVT_OK;
//     });
//   }

#ifndef OVT_SRC_JOBS_JOB_INTERNAL_HPP
#define OVT_SRC_JOBS_JOB_INTERNAL_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "ovt/cxx/jobs.hpp"
#include "ovt/jobs.h"

// The C handle. Cancellation is the one lock-free atomic inside `cancel`; everything else that a worker and
// another thread both touch is under `m`, which is never held while the job function or a callback runs.
struct ovt_Job {
  ovt::jobs::Cancel cancel;
  std::atomic<bool> busy{false};  // a call is using the job (§12.3.5: one blocking call at a time)

  mutable std::mutex m;
  uint64_t done = 0;
  uint64_t total = 0;
  const std::string* stage = nullptr;  // points into `stages`; nullptr = ""
  std::set<std::string> stages;        // interned stage names: out->stage stays valid until the job is freed
  ovt_progress_fn callback = nullptr;
  void* callback_user = nullptr;
  bool callback_called = false;
  std::chrono::steady_clock::time_point last_callback;
};

namespace ovt::jobs {

// Minimum interval between two progress callbacks (§12.3.5: at most every 100 ms).
inline constexpr std::chrono::milliseconds kProgressCallbackInterval{100};

// Runs `fn` for a blocking C entry point under `job` (nullable): on Pool::global(), waiting for it, or inline
// when the caller already is a pool worker. Maps ovt::Cancelled to OVT_E_CANCELLED, std::bad_alloc to
// OVT_E_NOMEM and every other exception to OVT_E_INTERNAL; nothing escapes (§12.3.1 rule 8). Returns
// OVT_E_CANCELLED without running `fn` when the job is already cancelled, and OVT_E_INVALID_ARG when the
// job is in use by another call.
ovt_status run(ovt_Job* job, std::string_view name, const Pool::JobFn& fn) noexcept;

// Records a progress report on `job` and invokes its callback, throttled to kProgressCallbackInterval.
void report(ovt_Job& job, const Progress& p) noexcept;

// ovt_job_set_progress_callback and ovt_job_reset.
void set_callback(ovt_Job& job, ovt_progress_fn fn, void* user) noexcept;
void reset(ovt_Job& job) noexcept;

// Fills `out` from the job's current progress (ovt_job_progress).
void snapshot(const ovt_Job& job, ovt_JobProgress& out) noexcept;

namespace detail {

// Largest explicit ovt_set_thread_count() value.
inline constexpr int kMaxThreads = 256;

ovt_status run_guarded(std::string_view name, Cancel& cancel, const Pool::JobFn& fn,
                       const Pool::ProgressSink& sink) noexcept;
ovt_status set_global_thread_count(int threads) noexcept;
int global_thread_count() noexcept;

}  // namespace detail

}  // namespace ovt::jobs

#endif  // OVT_SRC_JOBS_JOB_INTERNAL_HPP
