// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MockJob: a synthetic long-running job for the jobs tests (WI-1.512, D25). It sleeps in short slices, polls
// the cancellation token between slices and reports progress, so cancellation latency (NFR-007) and callback
// throttling (§12.3.5) can be tested before any real loop (index builder, WI-1.121) exists.

#ifndef OVT_TESTS_JOBS_MOCK_JOB_H
#define OVT_TESTS_JOBS_MOCK_JOB_H

#include <atomic>
#include <chrono>

#include "ovt/cxx/jobs.hpp"

namespace ovt::test {

struct MockJobOptions {
  std::chrono::milliseconds duration{2000};      // total work when not cancelled
  std::chrono::milliseconds slice{10};           // sleep between two cancellation polls
  std::chrono::milliseconds progress_every{10};  // progress report interval (rounded up to a slice)
  bool throw_on_cancel = false;                  // Cancel::throw_if_requested() instead of returning
  std::atomic<int>* reports = nullptr;           // counts progress reports when set
};

// Returns a Pool::JobFn that runs for opts.duration and returns OVT_OK, or OVT_E_CANCELLED (or throws
// ovt::Cancelled) within one slice of a cancel request. Progress: done = elapsed ms, total = duration ms,
// phase "mock".
ovt::jobs::Pool::JobFn make_mock_job(MockJobOptions opts);

}  // namespace ovt::test

#endif  // OVT_TESTS_JOBS_MOCK_JOB_H
