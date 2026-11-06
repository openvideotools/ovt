// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Acceptance tests of WI-1.512 (chapter 12 §12.6): cancellation latency through ovt_Job (NFR-007), progress
// callback throttling (§12.3.5) and the Cancelled → OVT_E_CANCELLED mapping at the job boundary (§12.3.1
// rule 8). They drive the synthetic MockJob, not the index builder (D25). The scenarios run in helper
// functions and the test cases only assert, which keeps each test case small.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include "jobs/job_internal.hpp"
#include "jobs/mock_job.h"
#include "ovt/jobs.h"

namespace {

using Clock = std::chrono::steady_clock;
using ovt::jobs::Cancel;
using ovt::jobs::Pool;
using std::chrono::milliseconds;

struct JobPtr {
  ovt_Job* job = nullptr;
  JobPtr() { REQUIRE(ovt_job_create(&job) == OVT_OK); }
  JobPtr(const JobPtr&) = delete;
  JobPtr& operator=(const JobPtr&) = delete;
  JobPtr(JobPtr&&) = delete;
  JobPtr& operator=(JobPtr&&) = delete;
  ~JobPtr() { ovt_job_free(job); }
};

ovt_JobProgress progress_of(const ovt_Job* job) {
  ovt_JobProgress p{};
  p.struct_size = sizeof(p);
  ovt_job_progress(job, &p);
  return p;
}

bool fraction_in_unit_range(const ovt_JobProgress& p) {
  return p.fraction >= 0.0 && p.fraction < 1.0;
}

struct CancelRun {
  ovt_status status = OVT_E_INTERNAL;
  milliseconds latency{};  // from ovt_job_cancel to the return of the blocking call
};

// Runs a 5 s MockJob through `job` and cancels it from another thread after `after`.
CancelRun run_and_cancel(ovt_Job* job, milliseconds after, bool throw_on_cancel = false) {
  ovt::test::MockJobOptions opts;
  opts.duration = milliseconds(5000);
  opts.throw_on_cancel = throw_on_cancel;
  Clock::time_point cancelled_at{};
  std::thread canceller([&] {
    std::this_thread::sleep_for(after);
    cancelled_at = Clock::now();
    ovt_job_cancel(job);
  });
  CancelRun r;
  r.status = ovt::jobs::run(job, "mock", ovt::test::make_mock_job(opts));
  const Clock::time_point returned_at = Clock::now();
  canceller.join();
  r.latency = std::chrono::duration_cast<milliseconds>(returned_at - cancelled_at);
  return r;
}

ovt_status run_mock(ovt_Job* job, milliseconds duration) {
  ovt::test::MockJobOptions opts;
  opts.duration = duration;
  return ovt::jobs::run(job, "mock", ovt::test::make_mock_job(opts));
}

struct CallbackCounter {
  std::atomic<int> calls{0};
  std::atomic<bool> fraction_in_range{true};
};

void count_callback(const ovt_JobProgress* p, void* user) {
  auto* c = static_cast<CallbackCounter*>(user);
  c->calls.fetch_add(1, std::memory_order_relaxed);
  if (p->struct_size != sizeof(ovt_JobProgress) || p->fraction < 0.0 || p->fraction > 1.0) {
    c->fraction_in_range.store(false);
  }
}

// A Pool task that loops on throw_if_requested() until cancelled; returns the job's final status.
ovt_status pool_task_throwing_cancelled() {
  Pool pool(2);
  std::atomic<bool> started{false};
  ovt::jobs::JobHandle h = pool.submit("throwing", [&](Cancel& c, const Pool::ProgressSink& /*report*/) -> ovt_status {
    started = true;
    for (;;) {
      c.throw_if_requested();
      std::this_thread::sleep_for(milliseconds(1));
    }
  });
  while (!started) {
    std::this_thread::sleep_for(milliseconds(1));
  }
  h.cancel.request();
  h.wait();
  return h.done() ? h.status() : OVT_E_INTERNAL;
}

template <class E>
ovt_status run_throwing(E e) {
  return ovt::jobs::run(nullptr, "throws", [&e](Cancel&, const Pool::ProgressSink&) -> ovt_status { throw e; });
}

// Holds `job` busy in a first call on another thread and returns what a second call gets meanwhile.
ovt_status second_call_while_busy(ovt_Job* job) {
  std::atomic<bool> inside{false};
  std::atomic<bool> release{false};
  ovt_status first_status = OVT_E_INTERNAL;
  std::thread first([&] {
    first_status = ovt::jobs::run(job, "first", [&](Cancel&, const Pool::ProgressSink&) {
      inside = true;
      while (!release) {
        std::this_thread::sleep_for(milliseconds(1));
      }
      return OVT_OK;
    });
  });
  while (!inside) {
    std::this_thread::sleep_for(milliseconds(1));
  }
  const ovt_status second = ovt::jobs::run(job, "second", [](Cancel&, const Pool::ProgressSink&) { return OVT_OK; });
  release = true;
  first.join();
  return first_status == OVT_OK ? second : OVT_E_INTERNAL;
}

}  // namespace

// Acceptance test 1, first part: latency and progress.
TEST_CASE("ovt_job_cancel from another thread ends a MockJob within 50 ms", "[jobs][fast]") {
  const JobPtr j;
  const CancelRun r = run_and_cancel(j.job, milliseconds(50));
  INFO("cancel latency " << r.latency.count() << " ms");
  CHECK(r.status == OVT_E_CANCELLED);
  CHECK(r.latency <= milliseconds(50));  // NFR-007
  CHECK(ovt_job_is_cancelled(j.job) == 1);
  const ovt_JobProgress p = progress_of(j.job);
  CHECK(fraction_in_unit_range(p));
  CHECK(std::string(p.stage) == "mock");
}

// Acceptance test 1, second part: reset and a second submission.
TEST_CASE("ovt_job_reset after a cancel allows a second submission", "[jobs][fast]") {
  const JobPtr j;
  REQUIRE(run_and_cancel(j.job, milliseconds(50)).status == OVT_E_CANCELLED);
  ovt_job_reset(j.job);
  CHECK(ovt_job_is_cancelled(j.job) == 0);
  CHECK(progress_of(j.job).fraction == -1.0);  // unknown after reset
  CHECK(run_mock(j.job, milliseconds(30)) == OVT_OK);
  CHECK(progress_of(j.job).fraction == 1.0);
}

// Acceptance test 2.
TEST_CASE("progress callback runs at most once per 100 ms", "[jobs][fast]") {
  const JobPtr j;
  CallbackCounter counter;
  ovt_job_set_progress_callback(j.job, &count_callback, &counter);
  std::atomic<int> reports{0};
  ovt::test::MockJobOptions opts;
  opts.duration = milliseconds(2000);
  opts.slice = milliseconds(1);
  opts.progress_every = milliseconds(1);
  opts.reports = &reports;

  CHECK(ovt::jobs::run(j.job, "mock", ovt::test::make_mock_job(opts)) == OVT_OK);
  INFO(reports.load() << " reports, " << counter.calls.load() << " callbacks");
  CHECK(reports.load() > 100);  // the mock really reported often
  CHECK(counter.calls.load() >= 2);
  CHECK(counter.calls.load() <= 21);  // ≤ 1 per 100 ms over 2 s (§12.3.5)
  CHECK(counter.fraction_in_range.load());
}

// Acceptance test 3.
TEST_CASE("ovt::Cancelled thrown in a Pool task becomes OVT_E_CANCELLED", "[jobs][fast]") {
  CHECK(pool_task_throwing_cancelled() == OVT_E_CANCELLED);
  const JobPtr j;
  CancelRun r;
  CHECK_NOTHROW(r = run_and_cancel(j.job, milliseconds(20), /*throw_on_cancel=*/true));
  CHECK(r.status == OVT_E_CANCELLED);
}

TEST_CASE("other exceptions end the job with OVT_E_INTERNAL or OVT_E_NOMEM", "[jobs][fast]") {
  CHECK(run_throwing(std::runtime_error("boom")) == OVT_E_INTERNAL);
  CHECK(run_throwing(std::bad_alloc()) == OVT_E_NOMEM);
  CHECK(run_throwing(42) == OVT_E_INTERNAL);
  CHECK(ovt::jobs::run(nullptr, "status", [](Cancel&, const Pool::ProgressSink&) { return OVT_E_CORRUPT; }) ==
        OVT_E_CORRUPT);
}

TEST_CASE("ovt_Job functions accept NULL", "[jobs][fast]") {
  CHECK(ovt_job_create(nullptr) == OVT_E_INVALID_ARG);
  ovt_job_cancel(nullptr);
  ovt_job_reset(nullptr);
  ovt_job_free(nullptr);
  ovt_job_set_progress_callback(nullptr, nullptr, nullptr);
  ovt_job_progress(nullptr, nullptr);
  CHECK(ovt_job_is_cancelled(nullptr) == 0);
}

TEST_CASE("ovt_job_progress honours the caller struct_size", "[jobs][fast]") {
  const JobPtr j;
  ovt_JobProgress small{};
  small.struct_size = sizeof(ovt_JobProgress) - 1;  // below v1: nothing is written
  small.fraction = 7.0;
  ovt_job_progress(j.job, &small);
  CHECK(small.struct_size == sizeof(ovt_JobProgress) - 1);
  CHECK(small.fraction == 7.0);
  ovt_JobProgress big{};
  big.struct_size = sizeof(ovt_JobProgress) + 64;  // newer caller: library writes its own size
  ovt_job_progress(j.job, &big);
  CHECK(big.struct_size == sizeof(ovt_JobProgress));
  CHECK(big.fraction == -1.0);
}

TEST_CASE("a fresh job reports unknown progress", "[jobs][fast]") {
  const JobPtr j;
  const ovt_JobProgress p = progress_of(j.job);
  CHECK(p.struct_size == sizeof(ovt_JobProgress));
  CHECK(p.fraction == -1.0);
  REQUIRE(p.stage != nullptr);
  CHECK(std::string(p.stage).empty());
}

TEST_CASE("cancel is idempotent and a cancelled job does not start", "[jobs][fast]") {
  const JobPtr j;
  ovt_job_cancel(j.job);
  ovt_job_cancel(j.job);
  CHECK(ovt_job_is_cancelled(j.job) == 1);
  bool ran = false;
  CHECK(ovt::jobs::run(j.job, "never", [&](Cancel&, const Pool::ProgressSink&) {
          ran = true;
          return OVT_OK;
        }) == OVT_E_CANCELLED);
  CHECK_FALSE(ran);
}

TEST_CASE("a job in use by a running call is rejected by a second call", "[jobs][fast]") {
  const JobPtr j;
  CHECK(second_call_while_busy(j.job) == OVT_E_INVALID_ARG);
}

TEST_CASE("stage pointers stay valid until the job is freed", "[jobs][fast]") {
  const JobPtr j;
  ovt_JobProgress first{};
  CHECK(ovt::jobs::run(j.job, "stages", [&](Cancel&, const Pool::ProgressSink& report) {
          report({1, 2, "parse"});
          first = progress_of(j.job);
          report({2, 2, "write"});
          return OVT_OK;
        }) == OVT_OK);
  CHECK(std::string(first.stage) == "parse");
  CHECK(std::string(progress_of(j.job).stage) == "write");
}
