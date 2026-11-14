// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Where the own kernels deliver diagnostics (chapter 9 §9.11.4). ovt_MetricApi has no diagnostics channel and a
// FrameScore carries none, so a kernel that has to report one (identity: VQ-DIFF-0001) writes it to a DiagSink the
// host installs, the same way the per-block maps reach a MapSink (#385). The host turns the entries into
// MetricsResult.diagnostics. A kernel finding no sink installed drops the diagnostic.
// SPEC-ASSUMPTION(#419): process-wide hook, because ovt_MetricApi.create() takes only options_json.

#ifndef OVT_SRC_METRICS_KERNELS_DIAG_SINK_H
#define OVT_SRC_METRICS_KERNELS_DIAG_SINK_H

#include <cstdint>
#include <string>
#include <string_view>

namespace ovt::metrics {

// One diagnostic of a kernel: `code` is the registry code ("VQ-DIFF-0001"), `severity` the ovt.fb.Severity value
// (0 Info, 1 Warning, 2 Error, 3 Fatal), `metric_id` the id of the reporting metric.
struct KernelDiagnostic {
  std::string_view metric_id;
  uint8_t severity = 0;
  std::string_view code;
  std::string message;
};

// May be called from the job's worker threads; the implementation must be thread-safe.
class DiagSink {
 public:
  DiagSink() = default;
  DiagSink(const DiagSink&) = delete;
  DiagSink& operator=(const DiagSink&) = delete;
  DiagSink(DiagSink&&) = delete;
  DiagSink& operator=(DiagSink&&) = delete;
  virtual ~DiagSink() = default;
  virtual void report(const KernelDiagnostic& diagnostic) = 0;
};

// Installs (or, with nullptr, removes) the process-wide sink. The caller keeps the sink alive until removed.
void set_diag_sink(DiagSink* sink) noexcept;
[[nodiscard]] DiagSink* diag_sink() noexcept;

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_KERNELS_DIAG_SINK_H
