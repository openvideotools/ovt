// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MetricJob state machine (chapter 9 §9.2.3; WI-3.300 step 4). See metric_job.h for what this skeleton does.

#include "metrics/host/metric_job.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

#include "metrics/host/plugin_registry.h"
#include "metrics/normalize/normalization_config.h"
#include "ovt/cxx/log.hpp"
#include "ovt_metrics_generated.h"

namespace ovt::metrics {

namespace {
constexpr std::string_view kModule = "metrics";
}  // namespace

std::string_view job_state_name(JobState s) noexcept {
  switch (s) {
    case JobState::Created:
      return "Created";
    case JobState::Probing:
      return "Probing";
    case JobState::Aligning:
      return "Aligning";
    case JobState::Scoring:
      return "Scoring";
    case JobState::Pooling:
      return "Pooling";
    case JobState::Done:
      return "Done";
    case JobState::Failed:
      return "Failed";
    case JobState::Cancelled:
      return "Cancelled";
  }
  return "Unknown";
}

std::string job_event_json(const JobEvent& e) {
  nlohmann::json j;  // std::map storage: keys come out sorted
  j["state"] = std::string(job_state_name(e.state));
  j["frames_done"] = e.frames_done;
  j["frames_total"] = e.frames_total;
  j["eta_seconds"] = e.eta_seconds;
  j["message"] = e.message;
  return j.dump();
}

std::string alignment_json(const AlignmentReport& r) {
  nlohmann::json j;
  j["status"] = r.status;
  j["offset_frames"] = r.offset_frames;
  j["confidence"] = r.confidence;
  j["shift_x"] = r.shift_x;
  j["shift_y"] = r.shift_y;
  return j.dump();
}

std::unique_ptr<MetricJob> MetricJob::create(const JobSpec& spec, jobs::Pool& pool, EventFn on_event,
                                             PluginRegistry& registry) {
  return std::unique_ptr<MetricJob>(new MetricJob(spec, pool, std::move(on_event), registry));
}

MetricJob::MetricJob(JobSpec spec, jobs::Pool& pool, EventFn on_event, PluginRegistry& registry)
    : spec_(std::move(spec)), pool_(pool), on_event_(std::move(on_event)), registry_(registry) {}

MetricJob::~MetricJob() = default;

void MetricJob::transition(JobState to, std::string message) {
  state_ = to;
  if (on_event_) {
    JobEvent e;
    e.state = to;
    e.message = std::move(message);
    on_event_(e);  // the C API wraps the user callback in a noexcept shim (§12.3.1 rule 8)
  }
}

ovt_status MetricJob::fail(ovt_status st, std::string message) {
  status_ = st;
  OVT_LOG(ovt::log::Level::Warn, kModule, "metrics job failed: {}", message);
  transition(JobState::Failed, std::move(message));
  return st;
}

bool MetricJob::cancelled(const ovt_Job* cancel) noexcept {
  return cancel != nullptr && ovt_job_is_cancelled(cancel) != 0;
}

ovt_status MetricJob::probe() {
  // 1. NormalizationConfig (§9.2.4): resolved once; the canonical string goes into every MetricsResult.
  Result<std::string> norm = resolve_normalization_config_json(spec_.normalization_json);
  if (!norm) {
    return fail(norm.status(), norm.message());
  }
  normalization_ = std::move(norm.value());
  if (spec_.subsample < 1) {
    return fail(OVT_E_INVALID_ARG, "JobSpec field subsample: must be >= 1");
  }
  if (spec_.frame_begin && spec_.frame_end && *spec_.frame_end < *spec_.frame_begin) {
    return fail(OVT_E_INVALID_ARG, "JobSpec field frames: end before begin");
  }
  // 2. Every metric id is registered (VQ-JOB-0002) and full-reference ids have a reference (VQ-JOB-0003).
  for (const MetricRequest& req : spec_.metrics) {
    const MetricPlugin* plugin = registry_.find(req.id);
    if (plugin == nullptr) {
      std::string ids;
      for (const MetricPlugin* p : registry_.plugins()) {
        ids += ids.empty() ? p->id : ", " + p->id;
      }
      return fail(OVT_E_INVALID_ARG, "VQ-JOB-0002 unknown metric id " + req.id + "; available: " + ids);
    }
    if (plugin->descriptor()->kind() == ovt::fb::MetricKind::FullReference && !spec_.ref_path) {
      return fail(OVT_E_INVALID_ARG, "VQ-JOB-0003 metric " + req.id + " is full-reference but no reference was given");
    }
    // Option objects are validated by the plugin's create() (rule 6); here only the JSON shape is checked.
    const auto parsed = nlohmann::json::parse(req.options_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return fail(OVT_E_INVALID_ARG, "metric " + req.id + ": options must be a JSON object");
    }
  }
  // The fixed temporal offset of the configuration is the alignment until the Aligning state runs (§9.3.1 step 8).
  const auto resolved = nlohmann::json::parse(normalization_);
  alignment_.status = "fixed";
  alignment_.offset_frames = resolved.at("temporal").at("offset_frames").get<int>();
  alignment_.shift_x = resolved.at("spatial").at("shift_x").get<int>();
  alignment_.shift_y = resolved.at("spatial").at("shift_y").get<int>();
  return OVT_OK;
}

ovt_status MetricJob::run(ovt_Job* cancel) {
  if (started_) {
    return OVT_E_INVALID_ARG;
  }
  started_ = true;
  (void)pool_;  // the chunked Scoring state of WI-3.306 submits its chunks here
  if (cancelled(cancel)) {
    status_ = OVT_E_CANCELLED;
    transition(JobState::Cancelled);
    return status_;
  }
  transition(JobState::Probing);
  if (const ovt_status st = probe(); st != OVT_OK) {
    return st;
  }
  if (!spec_.metrics.empty()) {
    // Opening the streams is the next step of Probing (§9.2.3: ovt_decoder_open, prefer_hw = 0); the decoder
    // arrives with WI-2.171 and the scoring loop with WI-3.301/WI-3.306.
    return fail(OVT_E_UNSUPPORTED, "scoring is not implemented yet (WI-3.301, WI-3.306): no metric can run");
  }
  for (const JobState next : {JobState::Aligning, JobState::Scoring, JobState::Pooling}) {
    if (cancelled(cancel)) {
      status_ = OVT_E_CANCELLED;
      transition(JobState::Cancelled);
      return status_;
    }
    transition(next);
  }
  results_.clear();  // no MetricRequest, no MetricsResult
  status_ = OVT_OK;
  transition(JobState::Done);
  return status_;
}

const std::vector<ovt::fb::MetricsResultT>& MetricJob::results() const noexcept {
  return results_;
}

const AlignmentReport& MetricJob::alignment() const noexcept {
  return alignment_;
}

JobState MetricJob::state() const noexcept {
  return state_;
}

ovt_status MetricJob::status() const noexcept {
  return status_;
}

const JobSpec& MetricJob::spec() const noexcept {
  return spec_;
}

const std::string& MetricJob::normalization() const noexcept {
  return normalization_;
}

}  // namespace ovt::metrics
