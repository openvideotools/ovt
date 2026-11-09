// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::metrics::MetricJob: one comparison and its state machine (chapter 9 §9.2.3; WI-3.300 step 4).
// Internal header of the metrics module (not installed). The declarations follow §9.2.3; AlignmentReport is
// §9.3.1's. Additions, recorded in the WI-3.300 PR: state(), status() and the `message` of a terminal event.
//
// WI-3.300 lands the skeleton: run() walks Created → Probing → Aligning → Scoring → Pooling → Done, emitting
// a JobEvent per transition and checking cancellation at every step, and Probing validates the request
// (NormalizationConfig resolved, every metric id registered, full-reference metrics given a reference). No
// stream is opened and no frame is scored yet: a job with metrics ends in Failed with OVT_E_UNSUPPORTED once
// it would have to decode (the decoders arrive with WI-2.171, the normaliser with WI-3.301, the kernels with
// WI-3.306); a job without metrics runs through to Done with no results, which is what the tests use.

#ifndef OVT_SRC_METRICS_HOST_METRIC_JOB_H
#define OVT_SRC_METRICS_HOST_METRIC_JOB_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "ovt/cxx/jobs.hpp"
#include "ovt/jobs.h"
#include "ovt/status.h"
#include "ovt_metrics_generated.h"

namespace ovt::metrics {

class PluginRegistry;

enum class JobState : uint8_t { Created, Probing, Aligning, Scoring, Pooling, Done, Failed, Cancelled };

// "Created", "Probing", ... as the JobEvent JSON spells them.
[[nodiscard]] std::string_view job_state_name(JobState s) noexcept;

struct MetricRequest {
  std::string id;
  std::string options_json = "{}";
};

struct JobSpec {
  std::optional<std::string> ref_path;  // required for FullReference ids
  std::string dist_path;
  int ref_track = 0;
  int dist_track = 0;
  std::vector<MetricRequest> metrics;
  std::string normalization_json = "{}";  // NormalizationConfig (§9.2.4); "{}" = defaults
  int threads = 0;                        // 0 = hardware_concurrency()
  int subsample = 1;                      // score every Nth frame pair (1 = all)
  std::optional<int> frame_begin;         // display indices [begin, end) on the reference
  std::optional<int> frame_end;
  std::string map_dir;  // where map blobs are written; empty = in-memory only
};

struct JobEvent {
  JobState state = JobState::Created;
  uint32_t frames_done = 0;
  uint32_t frames_total = 0;
  double eta_seconds = -1.0;  // -1 = unknown
  std::string message;        // empty, or the failure reason for Failed
};

// The JobEvent as the C API's event_json (ovt/metrics.h): keys sorted, no whitespace.
[[nodiscard]] std::string job_event_json(const JobEvent& e);

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

struct AlignmentReport {         // §9.3.1; serialised into normalization.alignment
  std::string status = "fixed";  // "ok" | "low_confidence" | "undetermined" | "fixed"
  int offset_frames = 0;
  double drift_per_1000 = 0.0;
  double confidence = 0.0;
  double best_score = 0.0;
  double second_score = 0.0;
  int shift_x = 0, shift_y = 0;  // spatial (§9.3.2)
  Rect ref_crop_detected, dist_crop_detected;
  std::vector<std::pair<int, int>> segments;  // (ref_rap_display_index, offset) when realign_per_gop
  uint32_t ref_frames = 0, dist_frames = 0, paired_frames = 0, skipped_ref_frames = 0, skipped_dist_frames = 0;
};

// The ovt_metrics_job_alignment() document: {"confidence":..,"offset_frames":..,"shift_x":..,"shift_y":..,
// "status":".."} with sorted keys.
[[nodiscard]] std::string alignment_json(const AlignmentReport& r);

class MetricJob {
 public:
  using EventFn = std::function<void(const JobEvent&)>;

  // `registry` is the metric registry Probing validates against (PluginRegistry::instance() for the C API).
  // Never fails: an invalid spec is reported by run(). `on_event` may be empty.
  static std::unique_ptr<MetricJob> create(const JobSpec& spec, jobs::Pool& pool, EventFn on_event,
                                           PluginRegistry& registry);

  ~MetricJob();
  MetricJob(const MetricJob&) = delete;
  MetricJob& operator=(const MetricJob&) = delete;
  MetricJob(MetricJob&&) = delete;
  MetricJob& operator=(MetricJob&&) = delete;

  // Blocking; call from a job thread. `cancel` (nullable) ends the job with OVT_E_CANCELLED. A job runs once:
  // a second call returns OVT_E_INVALID_ARG without changing the state.
  ovt_status run(ovt_Job* cancel);

  // One MetricsResult per MetricRequest (FlatBuffers object API; the C API packs them into the MetricsReport
  // without unpacking, which the clang analyzer cannot follow through the generated UnPack); filled when
  // state() == Done.
  [[nodiscard]] const std::vector<ovt::fb::MetricsResultT>& results() const noexcept;
  [[nodiscard]] const AlignmentReport& alignment() const noexcept;
  [[nodiscard]] JobState state() const noexcept;
  // The status run() returned (OVT_OK before run()).
  [[nodiscard]] ovt_status status() const noexcept;
  [[nodiscard]] const JobSpec& spec() const noexcept;
  // The resolved NormalizationConfig in canonical form; empty before Probing.
  [[nodiscard]] const std::string& normalization() const noexcept;

 private:
  MetricJob(JobSpec spec, jobs::Pool& pool, EventFn on_event, PluginRegistry& registry);
  void transition(JobState to, std::string message = {});
  [[nodiscard]] ovt_status fail(ovt_status st, std::string message);
  [[nodiscard]] static bool cancelled(const ovt_Job* cancel) noexcept;
  [[nodiscard]] ovt_status probe();

  JobSpec spec_;
  jobs::Pool& pool_;
  EventFn on_event_;
  PluginRegistry& registry_;
  JobState state_ = JobState::Created;
  ovt_status status_ = OVT_OK;
  bool started_ = false;
  std::string normalization_;
  AlignmentReport alignment_;
  std::vector<ovt::fb::MetricsResultT> results_;
};

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_HOST_METRIC_JOB_H
