// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The MetricsJobSpec JSON of ovt_metrics_job_create (chapter 12 §12.3.13) parsed into a JobSpec (chapter 9
// §9.2.3). WI-3.300 lands the strict shape check the C API skeleton needs; WI-3.312 extends it with the
// VQ-JOB-0001 diagnostics and the multi-distorted form. Internal header of the metrics module (not installed).
//
// Accepted keys (any other key is OVT_E_INVALID_ARG, chapter 04 FR-API-010):
//   reference  string (optional)            distorted  string or [string] (exactly one entry for now)
//   metrics    [ "id" | {"id":..,"options":{}} ]   normalize  object (NormalizationConfig, validated by the job)
//   align      "auto" | integer | "none"    maps       boolean (accepted; maps arrive with WI-3.316)
//   subsample  integer >= 1                 max_frames integer >= 0 (0 = all)
//   frames     "a-b" (display indices [a, b] on the reference, inclusive)   threads integer >= 0
// `align` N writes temporal.mode = "fixed", offset_frames = N into the normalisation object; "none" writes
// offset 0 (§9.3.1 step 8).

#ifndef OVT_SRC_METRICS_JOBSPEC_JSON_H
#define OVT_SRC_METRICS_JOBSPEC_JSON_H

#include <string_view>

#include "metrics/host/metric_job.h"
#include "ovt/cxx/result.hpp"

namespace ovt::metrics {

// Parses `text`. The error message names the offending field ("JobSpec field metrics[1].id: ...").
[[nodiscard]] Result<JobSpec> parse_job_spec(std::string_view text);

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_JOBSPEC_JSON_H
