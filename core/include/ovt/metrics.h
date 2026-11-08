// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/metrics.h
 * @brief Video quality metrics: descriptor listing and the metrics job (chapter 9 §9.10, chapter 12 §12.3.13).
 *
 * WI-3.300 skeleton: the declarations below are final; the job runs its state machine (§9.2.3) and validates
 * its inputs, but no metric is scored before the normalisation (WI-3.301) and kernel (WI-3.306) work items
 * land. The remaining §9.10 entry points arrive with their work items and are not declared yet:
 * ovt_metrics_job_set_alignment (WI-4.309); ovt_MapView, ovt_PlanePair, ovt_metrics_job_planes and
 * ovt_metrics_map (WI-3.316); ovt_metrics_push / ovt_metrics_finish (WI-3.533); the ovt_metrics_export_*
 * functions (WI-3.312); ovt_metrics_bd and ovt_BdResult (WI-3.313, WI-3.531).
 *
 * Threading (§12.3.1 rule 4): an ovt_MetricsJob handle is not thread-safe; ovt_metrics_job_run blocks and is
 * called from a worker thread; the event callback runs on that thread.
 */
#ifndef OVT_METRICS_H
#define OVT_METRICS_H

#include <stddef.h> /* NOLINT(modernize-deprecated-headers): a C11 header */
#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#include "ovt/jobs.h"
#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One comparison: one reference (nullable for no-reference metrics), one distorted stream, N metrics. */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_MetricsJob ovt_MetricsJob;

/**
 * Job event callback (§9.2.3 JobEvent as JSON). @p event_json is
 * `{"eta_seconds":-1.0,"frames_done":0,"frames_total":0,"message":"","state":"Probing"}` with the keys in
 * that (sorted) order; `state` is one of Created, Probing, Aligning, Scoring, Pooling, Done, Failed,
 * Cancelled. Called on the thread that runs ovt_metrics_job_run, at most 10 times per second plus once per
 * state change; the string is owned by the library and valid only during the call. MUST NOT throw and MUST
 * NOT call back into the job.
 */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef void (*ovt_MetricsEventFn)(const char* event_json, void* user);

/**
 * Lists every registered metric (built-in, plugin; sidecars from WI-4.304) as a JSON array of
 * ovt.fb.MetricDescriptor objects (core/schema/ovt_metrics.fbs field names, schema order, enum values as
 * names), sorted by id. Plugins are loaded on the first call from the plugin search path (§12.3.11 and
 * `$OVT_METRIC_PATH`). *out_descriptors_json is caller-owned (§12.3.1 rule 3a), NUL-terminated after
 * `size` bytes, and released with ovt_buffer_free(). OVT_E_INVALID_ARG when @p out_descriptors_json is NULL.
 */
OVT_API ovt_status ovt_metrics_list_descriptors(ovt_Buffer* out_descriptors_json);

/**
 * Creates a job from a MetricsJobSpec JSON object (§12.3.13):
 * `{"reference": "<path>", "distorted": ["<path>"], "metrics": [{"id": "psnr", "options": {...}}, "ssim"],
 *   "normalize": {NormalizationConfig, §9.2.4}, "align": "auto" | <int> | "none", "maps": false,
 *   "subsample": 1, "max_frames": 0, "frames": "0-299", "threads": 0}`.
 * Parsed strictly: an unknown key, a wrong type or an invalid NormalizationConfig is OVT_E_INVALID_ARG, and the
 * message naming the offending path is logged (ovt/log.h, module "metrics"). @p on_event (nullable) and
 * @p user are not owned by the
 * library and must stay valid until ovt_metrics_job_release(). On success *out_job is owned by the caller and
 * released with ovt_metrics_job_release(). OVT_E_UNSUPPORTED for more than one distorted stream (WI-3.312).
 */
OVT_API ovt_status ovt_metrics_job_create(const char* spec_json, ovt_MetricsEventFn on_event, void* user,
                                          ovt_MetricsJob** out_job);

/**
 * Runs the job to completion: Probing, Aligning, Scoring, Pooling, Done (§9.2.3), emitting one event per
 * transition. Blocking; call from a worker thread. @p cancel (nullable) is checked at least every 50 ms and
 * ends the job with OVT_E_CANCELLED. Returns OVT_E_INVALID_ARG for an unknown metric id or a full-reference
 * metric without a reference (VQ-JOB-0002, VQ-JOB-0003), and, in this skeleton, OVT_E_UNSUPPORTED once a
 * metric would have to be scored. A job runs once; a second call is OVT_E_INVALID_ARG.
 */
OVT_API ovt_status ovt_metrics_job_run(ovt_MetricsJob* job, ovt_Job* cancel);

/**
 * The results as a size-prefixed ovt.fb.MetricsReport FlatBuffer with file identifier "OVTR" (§9.9.3), one
 * MetricsResult per requested metric. Valid once the job is Done; OVT_E_INVALID_ARG before. *out_report_fb is
 * caller-owned and released with ovt_buffer_free().
 */
OVT_API ovt_status ovt_metrics_job_results(const ovt_MetricsJob* job, ovt_Buffer* out_report_fb);

/**
 * The alignment found so far as JSON `{"confidence":c,"offset_frames":o,"shift_x":x,"shift_y":y,
 * "status":"..."}` (§9.3.1 AlignmentReport; keys sorted). Meaningful once the Aligning state has completed;
 * before that it reports the configured fixed offset with status "fixed". *out_alignment_json is
 * caller-owned, NUL-terminated after `size` bytes, and released with ovt_buffer_free().
 */
OVT_API ovt_status ovt_metrics_job_alignment(const ovt_MetricsJob* job, ovt_Buffer* out_alignment_json);

/** Releases a job created by ovt_metrics_job_create(). The job MUST NOT be running. NULL is ignored. */
OVT_API void ovt_metrics_job_release(ovt_MetricsJob* job);

#ifdef __cplusplus
}
#endif

#endif /* OVT_METRICS_H */
