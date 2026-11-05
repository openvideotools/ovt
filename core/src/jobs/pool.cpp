// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::jobs::Cancel, JobHandle and Pool (chapter 7 §7.1.6, chapter 5 §5.5; WI-1.512).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "jobs/job_internal.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt/cxx/log.hpp"

namespace ovt::jobs {

namespace detail {

enum Phase : int { kQueued = 0, kRunning = 1, kDone = 2 };

struct JobState {
  uint64_t id = 0;
  std::string name;
  Cancel own_cancel;  // used unless the caller supplied a token
  Cancel* cancel = &own_cancel;
  Pool::JobFn fn;

  std::atomic<int> phase{kQueued};
  std::mutex m;  // guards progress and status; never held while the job function runs
  std::condition_variable cv;
  Progress progress;
  ovt_status status = OVT_E_INTERNAL;
};

namespace {

thread_local bool t_on_worker = false;

void finish(JobState& s, ovt_status status) {
  {
    const std::lock_guard<std::mutex> lock(s.m);
    s.status = status;
    s.phase.store(kDone, std::memory_order_release);
  }
  s.cv.notify_all();
}

}  // namespace

namespace {

// Out of line so that run_guarded stays a flat list of handlers.
void log_job_failure(std::string_view name, std::string_view what) noexcept {
  OVT_LOG(log::Level::Error, "jobs", "job '{}' failed: {}", name, what);
}

}  // namespace

ovt_status run_guarded(std::string_view name, Cancel& cancel, const Pool::JobFn& fn,
                       const Pool::ProgressSink& sink) noexcept {
  try {
    return fn(cancel, sink);
  } catch (const Cancelled&) {
    return OVT_E_CANCELLED;
  } catch (const std::bad_alloc&) {
    log_job_failure(name, "out of memory");
    return OVT_E_NOMEM;
  } catch (const std::exception& e) {
    log_job_failure(name, e.what());
    return OVT_E_INTERNAL;
  } catch (...) {
    log_job_failure(name, "non-standard exception");
    return OVT_E_INTERNAL;
  }
}

}  // namespace detail

Cancel& Cancel::never() noexcept {
  static Cancel token{Immortal{}};
  return token;
}

// ---- JobHandle -----------------------------------------------------------------------------------

void JobHandle::wait() {
  detail::JobState& s = *state;
  std::unique_lock<std::mutex> lock(s.m);
  while (s.phase.load(std::memory_order_acquire) != detail::kDone) {
    // A job cancelled while still queued never starts: claim it here, so the waiter returns within the
    // poll interval even when every worker is busy (NFR-007).
    if (s.cancel->requested()) {
      int expected = detail::kQueued;
      if (s.phase.compare_exchange_strong(expected, detail::kDone, std::memory_order_acq_rel)) {
        s.status = OVT_E_CANCELLED;
        s.fn = nullptr;  // never runs; its captures may reference the caller's stack
        lock.unlock();
        s.cv.notify_all();
        return;
      }
    }
    s.cv.wait_for(lock, std::chrono::milliseconds(5));
  }
}

bool JobHandle::done() const noexcept {
  return state->phase.load(std::memory_order_acquire) == detail::kDone;
}

ovt_status JobHandle::status() const noexcept {
  const std::lock_guard<std::mutex> lock(state->m);
  return state->phase.load(std::memory_order_acquire) == detail::kDone ? state->status : OVT_E_INTERNAL;
}

// ---- Pool ----------------------------------------------------------------------------------------

struct Pool::Impl {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::shared_ptr<detail::JobState>> queue;
  std::set<detail::JobState*> running;  // entries stay alive: the worker holds a shared_ptr meanwhile
  std::vector<std::thread> workers;
  bool stopping = false;
  std::atomic<uint64_t> next_id{1};
  // Returned by submit() when it cannot allocate (submit never throws). Pre-built for that reason.
  std::shared_ptr<detail::JobState> nomem = std::make_shared<detail::JobState>();

  void worker_loop() {
    detail::t_on_worker = true;
    for (;;) {
      std::shared_ptr<detail::JobState> s;
      {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty()) {
          return;  // stopping and drained
        }
        s = std::move(queue.front());
        queue.pop_front();
        int expected = detail::kQueued;
        if (!s->phase.compare_exchange_strong(expected, detail::kRunning, std::memory_order_acq_rel)) {
          continue;  // a waiter claimed it after a cancel
        }
        running.insert(s.get());
      }
      ovt_status status = OVT_E_INTERNAL;
      try {
        detail::JobState* raw = s.get();
        const ProgressSink sink = [raw](const Progress& p) {
          const std::lock_guard<std::mutex> lock(raw->m);
          raw->progress = p;
        };
        status = detail::run_guarded(s->name, *s->cancel, s->fn, sink);
      } catch (...) {  // building the sink failed: bad_alloc
        status = OVT_E_NOMEM;
      }
      s->fn = nullptr;  // release the captures before anyone is told the job is done
      {
        const std::lock_guard<std::mutex> lock(m);
        running.erase(s.get());
      }
      detail::finish(*s, status);
    }
  }
};

namespace {

unsigned resolve_threads(unsigned requested) noexcept {
  if (requested != 0) {
    return requested;
  }
  const unsigned hw = std::thread::hardware_concurrency();
  return std::clamp(hw, 1U, 16U);
}

}  // namespace

Pool::Pool(unsigned threads) : impl_(std::make_unique<Impl>()) {
  impl_->nomem->phase.store(detail::kDone);
  impl_->nomem->status = OVT_E_NOMEM;
  const unsigned n = resolve_threads(threads);
  impl_->workers.reserve(n);
  try {
    for (unsigned i = 0; i < n; ++i) {
      impl_->workers.emplace_back([impl = impl_.get()] { impl->worker_loop(); });
    }
  } catch (...) {
    {
      const std::lock_guard<std::mutex> lock(impl_->m);
      impl_->stopping = true;
    }
    impl_->cv.notify_all();
    for (std::thread& t : impl_->workers) {
      t.join();
    }
    throw;
  }
}

Pool::~Pool() {
  std::deque<std::shared_ptr<detail::JobState>> never_started;
  {
    const std::lock_guard<std::mutex> lock(impl_->m);
    impl_->stopping = true;
    for (detail::JobState* s : impl_->running) {
      s->cancel->request();
    }
    never_started.swap(impl_->queue);
  }
  impl_->cv.notify_all();
  // Do not touch s->cancel here: a waiter may already have claimed a cancelled queued job, and its
  // caller may then have freed the token. A job that never starts needs no token set.
  for (const std::shared_ptr<detail::JobState>& s : never_started) {
    int expected = detail::kQueued;
    if (s->phase.compare_exchange_strong(expected, detail::kDone, std::memory_order_acq_rel)) {
      s->fn = nullptr;
      detail::finish(*s, OVT_E_CANCELLED);
    }
  }
  for (std::thread& t : impl_->workers) {
    t.join();  // never detached (WI-1.512 notes)
  }
}

JobHandle Pool::submit(std::string name, JobFn fn) {
  return submit_impl(std::move(name), nullptr, std::move(fn));
}

JobHandle Pool::submit(std::string name, Cancel& cancel, JobFn fn) {
  return submit_impl(std::move(name), &cancel, std::move(fn));
}

JobHandle Pool::submit_impl(std::string name, Cancel* external, JobFn fn) {
  std::shared_ptr<detail::JobState> s;
  try {
    s = std::make_shared<detail::JobState>();
    s->id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);
    s->name = std::move(name);
    if (external != nullptr) {
      s->cancel = external;
    }
    s->fn = std::move(fn);
    bool rejected = false;
    {
      const std::lock_guard<std::mutex> lock(impl_->m);
      if (impl_->stopping) {
        rejected = true;
      } else {
        impl_->queue.push_back(s);
      }
    }
    if (rejected) {
      s->fn = nullptr;
      detail::finish(*s, OVT_E_CANCELLED);
    } else {
      impl_->cv.notify_one();
    }
  } catch (...) {  // bad_alloc from the state, the name or the queue node
    s = impl_->nomem;
  }
  detail::JobState* raw = s.get();
  std::function<Progress()> progress;
  try {
    progress = [s] {
      const std::lock_guard<std::mutex> lock(s->m);
      return s->progress;
    };
  } catch (...) {  // NOLINT(bugprone-empty-catch): leaves progress empty; callers check before use
  }
  return JobHandle{raw->id, *raw->cancel, std::move(progress), std::move(s)};
}

unsigned Pool::threads() const noexcept {
  return static_cast<unsigned>(impl_->workers.size());
}

bool Pool::on_worker_thread() noexcept {
  return detail::t_on_worker;
}

// ---- The process-wide pool -----------------------------------------------------------------------

namespace {

struct GlobalConfig {
  std::mutex m;
  unsigned requested = 0;  // 0 = default
  bool started = false;
};

GlobalConfig& global_config() {
  static GlobalConfig config;
  return config;
}

unsigned start_global_pool() {
  GlobalConfig& c = global_config();
  const std::lock_guard<std::mutex> lock(c.m);
  c.started = true;
  return resolve_threads(c.requested);
}

}  // namespace

Pool& Pool::global() {
  // A function-local static: created on first use, destroyed (threads joined) at process exit.
  static Pool pool(start_global_pool());
  return pool;
}

namespace detail {

ovt_status set_global_thread_count(int threads) noexcept {
  if (threads < 0 || threads > kMaxThreads) {
    return OVT_E_INVALID_ARG;
  }
  GlobalConfig& c = global_config();
  const std::lock_guard<std::mutex> lock(c.m);
  const unsigned wanted = resolve_threads(static_cast<unsigned>(threads));
  if (c.started) {
    // The pool has its threads already; only the same count is accepted.
    return wanted == resolve_threads(c.requested) ? OVT_OK : OVT_E_INVALID_ARG;
  }
  c.requested = static_cast<unsigned>(threads);
  return OVT_OK;
}

int global_thread_count() noexcept {
  GlobalConfig& c = global_config();
  const std::lock_guard<std::mutex> lock(c.m);
  return static_cast<int>(resolve_threads(c.requested));
}

}  // namespace detail

}  // namespace ovt::jobs
