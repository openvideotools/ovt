// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Running blocking C entry points under an ovt_Job (WI-1.512, chapter 12 §12.3.5): progress snapshot,
// callback throttling and the hand-off to ovt::jobs::Pool.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#include "jobs/job_internal.hpp"
#include "ovt/cxx/log.hpp"

namespace ovt::jobs {

namespace {

// Size of ovt_JobProgress as of its first version: every field to date.
constexpr uint32_t kJobProgressV1Size = sizeof(ovt_JobProgress);

double fraction_of(uint64_t done, uint64_t total) noexcept {
  if (total == 0) {
    return -1.0;
  }
  return std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0);
}

void fill(const ovt_Job& job, ovt_JobProgress& out) noexcept {
  out.struct_size = sizeof(ovt_JobProgress);
  out.fraction = fraction_of(job.done, job.total);
  out.done = job.done;
  out.total = job.total;
  out.stage = job.stage != nullptr ? job.stage->c_str() : "";
}

// Callbacks supplied by the caller must not throw (§12.3.1 rule 8); a throw terminates here.
void invoke(ovt_progress_fn fn, const ovt_JobProgress& p, void* user) noexcept {
  fn(&p, user);
}

// Clears `busy` on every exit path of run().
struct BusyGuard {
  explicit BusyGuard(ovt_Job* j) noexcept : job(j) {}
  ovt_Job* job;
  BusyGuard(const BusyGuard&) = delete;
  BusyGuard& operator=(const BusyGuard&) = delete;
  BusyGuard(BusyGuard&&) = delete;
  BusyGuard& operator=(BusyGuard&&) = delete;
  ~BusyGuard() {
    if (job != nullptr) {
      job->busy.store(false, std::memory_order_release);
    }
  }
};

}  // namespace

void report(ovt_Job& job, const Progress& p) noexcept {
  ovt_JobProgress copy{};
  ovt_progress_fn fn = nullptr;
  void* user = nullptr;
  {
    const std::lock_guard<std::mutex> lock(job.m);
    job.done = p.done;
    job.total = p.total;
    if (p.phase.empty()) {
      job.stage = nullptr;
    } else if (job.stage == nullptr || *job.stage != p.phase) {
      try {
        job.stage = &*job.stages.insert(p.phase).first;
      } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory, keep the previous stage name
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (job.callback != nullptr && (!job.callback_called || now - job.last_callback >= kProgressCallbackInterval)) {
      job.callback_called = true;
      job.last_callback = now;
      fn = job.callback;
      user = job.callback_user;
      fill(job, copy);
    }
  }
  if (fn != nullptr) {
    invoke(fn, copy, user);  // outside the lock: the callback may call ovt_job_progress()
  }
}

void snapshot(const ovt_Job& job, ovt_JobProgress& out) noexcept {
  // The caller owns `out` and sets struct_size (§12.3.1 rule 7, #88): write only what fits.
  const uint32_t caller_size = out.struct_size;
  if (caller_size < kJobProgressV1Size) {
    return;
  }
  ovt_JobProgress full{};
  {
    const std::lock_guard<std::mutex> lock(job.m);
    fill(job, full);
  }
  const uint32_t n = std::min<uint32_t>(caller_size, sizeof(ovt_JobProgress));
  std::memcpy(&out, &full, n);
  out.struct_size = n;
}

void set_callback(ovt_Job& job, ovt_progress_fn fn, void* user) noexcept {
  const std::lock_guard<std::mutex> lock(job.m);
  job.callback = fn;
  job.callback_user = user;
  job.callback_called = false;
}

void reset(ovt_Job& job) noexcept {
  job.cancel.reset();
  const std::lock_guard<std::mutex> lock(job.m);
  job.done = 0;
  job.total = 0;
  job.stage = nullptr;  // interned names stay until ovt_job_free (out->stage lifetime)
  job.callback_called = false;
}

namespace {

// Wraps `fn` so that its progress reports also reach the ovt_Job (snapshot and throttled callback).
Pool::JobFn with_job_progress(ovt_Job* job, const Pool::JobFn& fn) {
  return [job, &fn](Cancel& c, const Pool::ProgressSink& pool_sink) -> ovt_status {
    if (job == nullptr) {
      return fn(c, pool_sink);
    }
    const Pool::ProgressSink sink = [job, &pool_sink](const Progress& p) {
      pool_sink(p);
      report(*job, p);
    };
    return fn(c, sink);
  };
}

ovt_status dispatch(ovt_Job* job, std::string_view name, Cancel& cancel, const Pool::JobFn& fn) noexcept {
  try {
    const Pool::JobFn body = with_job_progress(job, fn);
    if (Pool::on_worker_thread()) {
      // Waiting on the pool from one of its own workers could deadlock a busy pool: run inline.
      return detail::run_guarded(name, cancel, body, [](const Progress& /*p*/) {});
    }
    const JobHandle handle = Pool::global().submit(std::string(name), cancel, body);
    handle.wait();
    return handle.status();
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {  // the pool could not start its threads (std::system_error)
    OVT_LOG(log::Level::Error, "jobs", "'{}': the worker pool is unavailable", name);
    return OVT_E_INTERNAL;
  }
}

}  // namespace

ovt_status run(ovt_Job* job, std::string_view name, const Pool::JobFn& fn) noexcept {
  if (job != nullptr) {
    bool expected = false;
    if (!job->busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      OVT_LOG(log::Level::Error, "jobs", "'{}': the ovt_Job is already used by another call", name);
      return OVT_E_INVALID_ARG;
    }
  }
  const BusyGuard guard(job);
  Cancel local;  // the token when the caller passed no job
  Cancel& cancel = job != nullptr ? job->cancel : local;
  if (cancel.requested()) {
    return OVT_E_CANCELLED;
  }
  return dispatch(job, name, cancel, fn);
}

}  // namespace ovt::jobs
