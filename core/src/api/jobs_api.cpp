// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/jobs.h entry points (chapter 12 §12.3.5, WI-1.512). No exception leaves these functions (§12.3.1
// rule 8): they only call noexcept helpers and nothrow allocation. The C declarations carry no exception
// specification, so the definitions cannot spell noexcept either (same as status.cpp).

#include <new>

#include "jobs/job_internal.hpp"
#include "ovt/jobs.h"

extern "C" {

OVT_API ovt_status ovt_job_create(ovt_Job** out) {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = new (std::nothrow) ovt_Job();
  return *out != nullptr ? OVT_OK : OVT_E_NOMEM;
}

OVT_API void ovt_job_set_progress_callback(ovt_Job* job, ovt_progress_fn fn, void* user) {
  if (job == nullptr) {
    return;
  }
  ovt::jobs::set_callback(*job, fn, user);
}

// Only an atomic store: callable from any thread and from a signal handler (WI-1.512 notes).
OVT_API void ovt_job_cancel(ovt_Job* job) {
  if (job != nullptr) {
    job->cancel.request();
  }
}

OVT_API int ovt_job_is_cancelled(const ovt_Job* job) {
  return job != nullptr && job->cancel.requested() ? 1 : 0;
}

OVT_API void ovt_job_progress(const ovt_Job* job, ovt_JobProgress* out) {
  if (job == nullptr || out == nullptr) {
    return;
  }
  ovt::jobs::snapshot(*job, *out);
}

OVT_API void ovt_job_reset(ovt_Job* job) {
  if (job == nullptr) {
    return;
  }
  ovt::jobs::reset(*job);
}

OVT_API void ovt_job_free(ovt_Job* job) {
  delete job;  // NOLINT(cppcoreguidelines-owning-memory): C handle
}

OVT_API ovt_status ovt_set_thread_count(int threads) {
  return ovt::jobs::detail::set_global_thread_count(threads);
}

OVT_API int ovt_thread_count(void) {
  return ovt::jobs::detail::global_thread_count();
}

}  // extern "C"
