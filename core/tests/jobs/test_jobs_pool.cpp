// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::jobs::Pool and Cancel (chapter 7 §7.1.6, WI-1.512). The scenarios run in helper functions and the test
// cases only assert, which keeps each test case small.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "jobs/job_internal.hpp"
#include "jobs/mock_job.h"
#include "ovt/cxx/jobs.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/jobs.h"

namespace {

using ovt::jobs::Cancel;
using ovt::jobs::JobHandle;
using ovt::jobs::Pool;
using std::chrono::milliseconds;

void wait_for(const std::atomic<bool>& flag) {
  while (!flag) {
    std::this_thread::sleep_for(milliseconds(1));
  }
}

struct ManyJobs {
  int ran = 0;
  int ok = 0;
  int phase_seen = 0;
  bool all_on_workers = true;
  bool distinct_ids = false;
};

ManyJobs run_many(Pool& pool, int n) {
  std::atomic<int> ran{0};
  std::atomic<bool> on_workers{true};
  std::vector<JobHandle> handles;
  handles.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    handles.push_back(pool.submit("job" + std::to_string(i), [&](Cancel&, const Pool::ProgressSink& report) {
      if (!Pool::on_worker_thread()) {
        on_workers = false;
      }
      report({1, 1, "count"});
      ran.fetch_add(1);
      return OVT_OK;
    }));
  }
  ManyJobs r;
  for (JobHandle& h : handles) {
    h.wait();
    r.ok += h.status() == OVT_OK ? 1 : 0;
    r.phase_seen += h.progress().phase == "count" ? 1 : 0;
  }
  r.ran = ran.load();
  r.all_on_workers = on_workers.load();
  r.distinct_ids = handles.front().id != handles.back().id;
  return r;
}

struct QueuedCancel {
  ovt_status queued_status = OVT_E_INTERNAL;
  ovt_status blocker_status = OVT_E_INTERNAL;
  milliseconds wait_time{};
  bool queued_ran = false;
};

// On a one-thread pool, a job queued behind a blocker is cancelled; waiting on it must not wait for the
// blocker.
QueuedCancel cancel_queued_job() {
  Pool pool(1);
  std::atomic<bool> release{false};
  JobHandle blocker = pool.submit("blocker", [&](Cancel&, const Pool::ProgressSink&) {
    wait_for(release);
    return OVT_OK;
  });
  QueuedCancel r;
  JobHandle queued = pool.submit("queued", [&](Cancel&, const Pool::ProgressSink&) {
    r.queued_ran = true;
    return OVT_OK;
  });
  queued.cancel.request();
  const auto t0 = std::chrono::steady_clock::now();
  queued.wait();
  r.wait_time = std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - t0);
  r.queued_status = queued.status();
  release = true;
  blocker.wait();
  r.blocker_status = blocker.status();
  return r;
}

// Destroys a one-thread pool while a 10 s job runs and another waits in the queue.
std::vector<JobHandle> destroy_busy_pool() {
  std::atomic<bool> started{false};
  std::vector<JobHandle> handles;
  Pool pool(1);
  ovt::test::MockJobOptions opts;
  opts.duration = milliseconds(10000);
  handles.push_back(pool.submit("long", [&, fn = ovt::test::make_mock_job(opts)](Cancel& c, const auto& r) {
    started = true;
    return fn(c, r);
  }));
  handles.push_back(pool.submit("never", [](Cancel&, const Pool::ProgressSink&) { return OVT_OK; }));
  wait_for(started);
  return handles;  // ~Pool runs here and must not take 10 s
}

// A queued job is cancelled and claimed by its waiter, its external token is freed, and then the
// pool is destroyed while the stale queue entry is still there. Fails under ASan if ~Pool touches
// the freed token.
bool destroy_pool_after_freeing_claimed_token() {
  auto pool = std::make_unique<Pool>(1);
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  JobHandle blocker = pool->submit("block", [&](Cancel& c, const Pool::ProgressSink&) {
    started = true;
    while (!release && !c.requested()) {
      std::this_thread::sleep_for(milliseconds(1));
    }
    return OVT_OK;
  });
  wait_for(started);
  ovt_status queued_status = OVT_OK;
  {
    auto token = std::make_unique<Cancel>();
    JobHandle h = pool->submit("queued", *token, [](Cancel&, const Pool::ProgressSink&) { return OVT_OK; });
    token->request();
    h.wait();
    queued_status = h.status();
  }  // token freed, as ovt_job_free would
  pool.reset();
  return queued_status == OVT_E_CANCELLED && blocker.done();
}

int error_lines_for_throwing_job(ovt_status& status) {
  std::atomic<int> errors{0};
  ovt::log::set_sink([&](ovt::log::Level l, std::string_view module, std::string_view, std::string_view msg) {
    if (l == ovt::log::Level::Error && module == "jobs" && msg.find("bad job") != std::string_view::npos) {
      errors.fetch_add(1);
    }
  });
  {
    Pool pool(1);
    JobHandle h =
        pool.submit("bad job", [](Cancel&, const Pool::ProgressSink&) -> ovt_status { throw std::logic_error("x"); });
    h.wait();
    status = h.status();
  }
  ovt::log::set_sink({});
  return errors.load();
}

}  // namespace

TEST_CASE("Cancel token", "[jobs][fast]") {
  Cancel c;
  CHECK_FALSE(c.requested());
  c.request();
  c.request();
  CHECK(c.check());
  CHECK_THROWS_AS(c.throw_if_requested(), ovt::Cancelled);
  c.reset();
  CHECK_NOTHROW(c.throw_if_requested());
}

TEST_CASE("Cancel::never() is shared and cannot be requested", "[jobs][fast]") {
  Cancel::never().request();
  CHECK_FALSE(Cancel::never().requested());
  CHECK(&Cancel::never() == &Cancel::never());
}

TEST_CASE("Pool runs every submitted job on its workers", "[jobs][fast]") {
  Pool pool(3);
  CHECK(pool.threads() == 3);
  const ManyJobs r = run_many(pool, 50);
  CHECK(r.ran == 50);
  CHECK(r.ok == 50);
  CHECK(r.phase_seen == 50);
  CHECK(r.all_on_workers);
  CHECK(r.distinct_ids);
  CHECK_FALSE(Pool::on_worker_thread());
}

TEST_CASE("Pool(0) uses the hardware concurrency capped at 16", "[jobs][fast]") {
  const Pool pool(0);
  CHECK(pool.threads() >= 1);
  CHECK(pool.threads() <= 16);
}

TEST_CASE("a queued job cancelled before it starts returns at once", "[jobs][fast]") {
  const QueuedCancel r = cancel_queued_job();
  CHECK(r.queued_status == OVT_E_CANCELLED);
  CHECK(r.wait_time <= milliseconds(50));
  CHECK(r.blocker_status == OVT_OK);
  CHECK_FALSE(r.queued_ran);
}

TEST_CASE("destroying a Pool cancels running and queued jobs and joins", "[jobs][fast]") {
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<JobHandle> handles = destroy_busy_pool();
  CHECK(std::chrono::steady_clock::now() - t0 < milliseconds(5000));
  REQUIRE(handles.size() == 2);
  CHECK(handles[0].done());
  CHECK(handles[0].status() == OVT_E_CANCELLED);
  CHECK(handles[1].done());
  CHECK(handles[1].status() == OVT_E_CANCELLED);
}

TEST_CASE("destroying a Pool after a claimed queued job freed its token is safe", "[jobs][fast]") {
  CHECK(destroy_pool_after_freeing_claimed_token());
}

TEST_CASE("a throwing job logs at level 4", "[jobs][fast]") {
  ovt_status status = OVT_OK;
  CHECK(error_lines_for_throwing_job(status) == 1);
  CHECK(status == OVT_E_INTERNAL);
}

TEST_CASE("jobs::run from a pool worker runs inline", "[jobs][fast]") {
  Pool pool(1);  // a nested wait on a one-thread pool would deadlock
  ovt_status inner = OVT_E_INTERNAL;
  std::atomic<bool> inner_on_worker{false};
  JobHandle h = pool.submit("outer", [&](Cancel&, const Pool::ProgressSink&) {
    inner = ovt::jobs::run(nullptr, "inner", [&](Cancel&, const Pool::ProgressSink&) {
      inner_on_worker = Pool::on_worker_thread();
      return OVT_OK;
    });
    return OVT_OK;
  });
  h.wait();
  CHECK(h.status() == OVT_OK);
  CHECK(inner == OVT_OK);
  CHECK(inner_on_worker);
}

TEST_CASE("ovt_set_thread_count rejects bad counts", "[jobs][fast]") {
  CHECK(ovt_set_thread_count(-1) == OVT_E_INVALID_ARG);
  CHECK(ovt_set_thread_count(ovt::jobs::detail::kMaxThreads + 1) == OVT_E_INVALID_ARG);
}

TEST_CASE("ovt_set_thread_count sizes the global pool before it starts", "[jobs][fast]") {
  // Another test case of this executable may have started the global pool already (when the binary is run
  // without CTest); then only its own count is accepted.
  const int before = ovt_thread_count();
  const ovt_status st = ovt_set_thread_count(3);
  const int expected = st == OVT_OK ? 3 : before;
  CHECK(ovt_thread_count() == expected);
  CHECK(static_cast<int>(Pool::global().threads()) == expected);  // starts the pool
  CHECK(ovt_set_thread_count(expected) == OVT_OK);
  CHECK(ovt_set_thread_count(expected + 1) == OVT_E_INVALID_ARG);
  CHECK(ovt_thread_count() == expected);
}
