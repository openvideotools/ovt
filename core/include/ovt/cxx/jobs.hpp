// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::jobs::{Cancel, Progress, JobHandle, Pool} (chapter 7 §7.1.6, chapter 5 §5.5; WI-1.512). C++ helper
// header (D22): no ABI promise, never included from a C header, not installed.
//
// The declarations follow chapter 7 §7.1.6. Additions, all recorded in the WI-1.512 PR: Cancel::check() and
// Cancel::reset() (the ovt_job_reset path), JobHandle::status(), the Pool::submit overload that takes the
// caller's Cancel token (the ovt_Job path), and deleted copy/move where an object holds atomics or threads.

#ifndef OVT_CXX_JOBS_HPP
#define OVT_CXX_JOBS_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "ovt/cxx/cancel.hpp"
#include "ovt/status.h"

namespace ovt::jobs {

// Cancellation token, checked cooperatively at least every 50 ms of work (§0.7.2, NFR-007) by the index,
// decode, container and metrics loops. Passed by reference down every long-running call chain; functions
// MUST NOT store it beyond the call (§7.1.6 rule 1). request() and requested() only touch one lock-free
// atomic, so they never take a lock the worker could hold and request() is async-signal-safe.
class Cancel {
 public:
  Cancel() noexcept = default;
  Cancel(const Cancel&) = delete;
  Cancel& operator=(const Cancel&) = delete;
  Cancel(Cancel&&) = delete;
  Cancel& operator=(Cancel&&) = delete;
  ~Cancel() = default;

  // True once request() was called (acquire load).
  [[nodiscard]] bool requested() const noexcept { return flag_.load(std::memory_order_acquire); }
  // Same as requested(); the spelling the loop helpers of later WIs use: `if (cancel.check()) return …;`.
  [[nodiscard]] bool check() const noexcept { return requested(); }
  // Idempotent and thread-safe. A no-op on never().
  void request() noexcept {
    if (!immortal_) {
      flag_.store(true, std::memory_order_release);
    }
  }
  // Throws ovt::Cancelled once requested (caught at the job boundary and mapped to OVT_E_CANCELLED).
  void throw_if_requested() const {
    if (requested()) {
      throw Cancelled();
    }
  }
  // Clears the flag. Only while no task uses the token (ovt_job_reset).
  void reset() noexcept { flag_.store(false, std::memory_order_release); }

  // Shared token that is never requested (for synchronous CLI paths).
  static Cancel& never() noexcept;

 private:
  struct Immortal {};
  explicit Cancel(Immortal /*tag*/) noexcept : immortal_(true) {}

  static_assert(std::atomic<bool>::is_always_lock_free, "ovt_job_cancel must stay async-signal-safe");
  std::atomic<bool> flag_{false};
  bool immortal_ = false;
};
using CancelToken = Cancel;  // alias used in some chapters

struct Progress {
  uint64_t done = 0, total = 0;  // total = 0 → indeterminate
  std::string phase;
};

namespace detail {
struct JobState;
}  // namespace detail

struct JobHandle {
  uint64_t id;
  Cancel& cancel;                      // request() cancels the job
  std::function<Progress()> progress;  // snapshot, callable from any thread
  void wait() const;                   // blocks until finished, or until cancelled before it started
  [[nodiscard]] bool done() const noexcept;
  // The job's result once done(): OVT_E_CANCELLED for ovt::Cancelled or a job cancelled before it started,
  // OVT_E_NOMEM for std::bad_alloc, OVT_E_INTERNAL for any other exception; OVT_E_INTERNAL while running.
  [[nodiscard]] ovt_status status() const noexcept;

  std::shared_ptr<detail::JobState> state;  // keeps cancel and the result alive; not for direct use
};

class Pool {
 public:
  using ProgressSink = std::function<void(const Progress&)>;
  using JobFn = std::function<ovt_status(Cancel&, ProgressSink)>;

  explicit Pool(unsigned threads = 0);  // 0 = hardware_concurrency() capped at 16 (chapter 12 §12.1.2 --threads)
  ~Pool();                              // requests cancel on every queued and running job, then joins
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  Pool(Pool&&) = delete;
  Pool& operator=(Pool&&) = delete;

  // The function receives the job's Cancel token and a progress sink; it MUST check cancel at least every
  // 50 ms. Never throws (§7.1.6 rule 2): a function that throws ovt::Cancelled ends the job with
  // OVT_E_CANCELLED, one that throws anything else ends it with OVT_E_INTERNAL and a level-4 log line.
  JobHandle submit(std::string name, JobFn fn);
  // Same, with a token owned by the caller, which MUST outlive the job (the ovt_Job path of the C API).
  JobHandle submit(std::string name, Cancel& cancel, JobFn fn);

  [[nodiscard]] unsigned threads() const noexcept;
  // True when the calling thread is a worker of any Pool. A blocking entry point that would wait on the
  // pool from one of its own workers runs its work inline instead (no deadlock on a busy pool).
  [[nodiscard]] static bool on_worker_thread() noexcept;
  // The process-wide pool used by the C API (ovt_Job) and the GUI. Created on first use with the count
  // set by ovt_set_thread_count(); destroyed, and its threads joined, at process exit (never detached).
  static Pool& global();

 private:
  JobHandle submit_impl(std::string name, Cancel* external, JobFn fn);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ovt::jobs

#endif  // OVT_CXX_JOBS_HPP
