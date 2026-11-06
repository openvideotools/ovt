// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/jobs.h
 * @brief Cancellation and progress of long-running calls, and the process-wide worker pool
 *        (chapter 12 §12.3.5, chapter 04 FR-API-004, NFR-007).
 *
 * A job is passed to one blocking call at a time; passing the same job to two concurrent calls is
 * undefined (the library rejects it with OVT_E_INVALID_ARG where it can detect it). Cancellation is
 * cooperative: the running call checks the job at least every 50 ms of work and returns
 * OVT_E_CANCELLED, leaving its handle in a valid state.
 */
#ifndef OVT_JOBS_H
#define OVT_JOBS_H

#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque job object. Created with ovt_job_create(), released with ovt_job_free(). */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_Job ovt_Job;

/** Progress snapshot (chapter 12 §12.3.1 rule 7: begins with struct_size, fields are only appended). */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_JobProgress {
  uint32_t struct_size; /**< Set by the caller before ovt_job_progress(); see there. Library-built callback arguments carry sizeof. */
  double fraction;      /**< 0.0 to 1.0, or -1.0 when the total is unknown. */
  uint64_t done;        /**< Work units done. */
  uint64_t total;       /**< Work units in total; 0 when unknown. */
  const char* stage;    /**< Stage name, never NULL ("" before the first report); owned by the job. */
} ovt_JobProgress;

/**
 * Progress callback. Called from a worker thread, at most once every 100 ms. @p p is owned by the
 * library and valid only for the duration of the call. MUST NOT throw; MAY call ovt_job_progress(),
 * ovt_job_cancel() and ovt_job_is_cancelled() on the same job.
 */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef void (*ovt_progress_fn)(const ovt_JobProgress* p, void* user);

/**
 * Creates a job. On success *@p out receives a job owned by the caller, released with ovt_job_free().
 * Returns OVT_E_INVALID_ARG when @p out is NULL and OVT_E_NOMEM when allocation fails.
 */
OVT_API ovt_status ovt_job_create(ovt_Job** out);

/**
 * Sets (or, with @p fn NULL, clears) the progress callback of @p job (chapter 04 FR-API-004). @p user is
 * not owned by the library. Only when no call is using the job. A NULL @p job is ignored.
 */
OVT_API void ovt_job_set_progress_callback(ovt_Job* job, ovt_progress_fn fn, void* user);

/**
 * Requests cancellation. Thread-safe and idempotent; only sets an atomic flag, so it may be called from a
 * signal handler. The running call returns OVT_E_CANCELLED within 50 ms of work (NFR-007). A NULL
 * @p job is ignored.
 */
OVT_API void ovt_job_cancel(ovt_Job* job);

/** Returns 1 once ovt_job_cancel() was called (until ovt_job_reset()), else 0; 0 for NULL. Thread-safe. */
OVT_API int ovt_job_is_cancelled(const ovt_Job* job);

/**
 * Writes a thread-safe snapshot of the job's progress to *@p out, which the caller owns; out->stage is
 * owned by the job and stays valid until the job is freed. NULL arguments are ignored.
 *
 * The caller MUST set out->struct_size = sizeof(ovt_JobProgress) first (chapter 12 §12.3.1 rule 7). The
 * library writes only the leading fields that fit in that size and then sets out->struct_size to the
 * smaller of the caller's value and its own sizeof. A struct_size below the first version (the size of
 * the struct as of v1) writes nothing.
 */
OVT_API void ovt_job_progress(const ovt_Job* job, ovt_JobProgress* out);

/** Clears the cancelled flag and the progress. Only when no call is using the job. NULL is ignored. */
OVT_API void ovt_job_reset(ovt_Job* job);

/** Releases a job created by ovt_job_create(). The job MUST NOT be in use by a running call. NULL is ignored. */
OVT_API void ovt_job_free(ovt_Job* job);

/**
 * Sets the size of the process-wide worker pool used by all handles. 0 = hardware concurrency capped
 * at 16. Call before creating handles: once the pool has started, a different count returns
 * OVT_E_INVALID_ARG and the pool keeps its size. A count below 0 or above 256 returns OVT_E_INVALID_ARG.
 */
OVT_API ovt_status ovt_set_thread_count(int threads);

/** Returns the worker count the pool has, or will have once it starts. Never fails. */
OVT_API int ovt_thread_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OVT_JOBS_H */
