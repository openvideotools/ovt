// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "jobs/mock_job.h"

#include <algorithm>
#include <thread>

namespace ovt::test {

ovt::jobs::Pool::JobFn make_mock_job(MockJobOptions opts) {
  return [opts](ovt::jobs::Cancel& cancel, const ovt::jobs::Pool::ProgressSink& report) -> ovt_status {
    using Clock = std::chrono::steady_clock;
    const auto slice = std::max(std::chrono::milliseconds(1), std::min(opts.slice, opts.progress_every));
    const auto start = Clock::now();
    auto last_report = start - opts.progress_every;
    const auto total = static_cast<uint64_t>(opts.duration.count());
    for (;;) {
      if (opts.throw_on_cancel) {
        cancel.throw_if_requested();
      } else if (cancel.check()) {
        return OVT_E_CANCELLED;
      }
      const auto now = Clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
      if (elapsed >= opts.duration) {
        report({total, total, "mock"});
        return OVT_OK;
      }
      if (now - last_report >= opts.progress_every) {
        last_report = now;
        report({static_cast<uint64_t>(elapsed.count()), total, "mock"});
        if (opts.reports != nullptr) {
          opts.reports->fetch_add(1, std::memory_order_relaxed);
        }
      }
      std::this_thread::sleep_for(slice);
    }
  };
}

}  // namespace ovt::test
