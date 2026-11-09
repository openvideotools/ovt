// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/metrics.h entry points (chapter 9 §9.10, chapter 12 §12.3.13; WI-3.300 skeleton, WI-3.316 adds the map and
// plane calls). No exception leaves these functions (§12.3.1 rule 8): every body runs inside a catch-all that
// maps std::bad_alloc to OVT_E_NOMEM and anything else to OVT_E_INTERNAL. The C declarations carry no exception
// specification, so the definitions cannot spell noexcept either (same as jobs_api.cpp).

#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "api/buffer.hpp"
#include "metrics/host/metric_job.h"
#include "metrics/host/plugin_registry.h"
#include "metrics/jobspec_json.h"
#include "ovt/cxx/jobs.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/metrics.h"
#include "ovt_metrics_generated.h"

#ifndef OVT_VERSION_STRING_VALUE
#error "OVT_VERSION_STRING_VALUE must be defined by core/src/metrics/CMakeLists.txt"
#endif

// The C handle (chapter 12 §12.3.1 rule 4: not thread-safe; one blocking call at a time).
struct ovt_MetricsJob {
  std::unique_ptr<ovt::metrics::MetricJob> job;
  ovt_MetricsEventFn on_event = nullptr;
  void* user = nullptr;
  std::string reference_name;
  std::string distorted_name;
};

namespace {

constexpr std::string_view kModule = "metrics";

template <class F>
ovt_status guarded(F&& fn) noexcept {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (const std::exception& e) {
    OVT_LOG(ovt::log::Level::Error, kModule, "metrics C API: {}", e.what());
    return OVT_E_INTERNAL;
  } catch (...) {
    return OVT_E_INTERNAL;
  }
}

// The user's callback runs inside a noexcept shim: a throw terminates (§12.3.1 rule 8).
void deliver_event(const ovt_MetricsJob& handle, const ovt::metrics::JobEvent& e) noexcept {
  if (handle.on_event == nullptr) {
    return;
  }
  try {
    const std::string json = ovt::metrics::job_event_json(e);
    handle.on_event(json.c_str(), handle.user);
  } catch (...) {
    // Building the event text allocates; an event that cannot be built is dropped rather than terminating the job.
  }
}

std::string basename_of(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

}  // namespace

extern "C" {

OVT_API ovt_status ovt_metrics_list_descriptors(ovt_Buffer* out_descriptors_json) {
  if (out_descriptors_json == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    const std::string json = ovt::metrics::PluginRegistry::instance().descriptors_json();
    return ovt::api::make_buffer(json, out_descriptors_json);
  });
}

OVT_API ovt_status ovt_metrics_job_create(const char* spec_json, ovt_MetricsEventFn on_event, void* user,
                                          ovt_MetricsJob** out_job) {
  if (spec_json == nullptr || out_job == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out_job = nullptr;
  return guarded([&] {
    ovt::Result<ovt::metrics::JobSpec> spec = ovt::metrics::parse_job_spec(spec_json);
    if (!spec) {
      OVT_LOG(ovt::log::Level::Warn, kModule, "ovt_metrics_job_create: {}", spec.message());
      return spec.status();
    }
    auto handle = std::make_unique<ovt_MetricsJob>();
    handle->on_event = on_event;
    handle->user = user;
    handle->reference_name = spec.value().ref_path ? basename_of(*spec.value().ref_path) : std::string();
    handle->distorted_name = basename_of(spec.value().dist_path);
    ovt_MetricsJob* raw = handle.get();
    handle->job = ovt::metrics::MetricJob::create(
        spec.value(), ovt::jobs::Pool::global(), [raw](const ovt::metrics::JobEvent& e) { deliver_event(*raw, e); },
        ovt::metrics::PluginRegistry::instance());
    *out_job = handle.release();
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_metrics_job_run(ovt_MetricsJob* job, ovt_Job* cancel) {
  if (job == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] { return job->job->run(cancel); });
}

OVT_API ovt_status ovt_metrics_job_results(const ovt_MetricsJob* job, ovt_Buffer* out_report_fb) {
  if (job == nullptr || out_report_fb == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    if (job->job->state() != ovt::metrics::JobState::Done) {
      return OVT_E_INVALID_ARG;
    }
    // One MetricsReport (§9.9.3) holding a copy of every MetricsResult the job produced.
    flatbuffers::FlatBufferBuilder fbb;
    std::vector<flatbuffers::Offset<ovt::fb::MetricsResult>> results;
    for (const ovt::fb::MetricsResultT& r : job->job->results()) {
      results.push_back(ovt::fb::MetricsResult::Pack(fbb, &r));
    }
    const auto results_vec = fbb.CreateVector(results);
    const auto ref_name = fbb.CreateString(job->reference_name);
    const auto dist_name = fbb.CreateString(job->distorted_name);
    const auto version = fbb.CreateString(std::string("ovt ") + OVT_VERSION_STRING_VALUE);
    ovt::fb::MetricsReportBuilder report(fbb);
    report.add_schema_version(1);
    report.add_results(results_vec);
    report.add_reference_name(ref_name);
    report.add_distorted_name(dist_name);
    report.add_ovt_version(version);
    fbb.FinishSizePrefixed(report.Finish(), ovt::fb::MetricsReportIdentifier());
    return ovt::api::make_buffer(fbb.GetBufferPointer(), fbb.GetSize(), out_report_fb);
  });
}

OVT_API ovt_status ovt_metrics_job_alignment(const ovt_MetricsJob* job, ovt_Buffer* out_alignment_json) {
  if (job == nullptr || out_alignment_json == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    const std::string json = ovt::metrics::alignment_json(job->job->alignment());
    return ovt::api::make_buffer(json, out_alignment_json);
  });
}

OVT_API void ovt_metrics_job_release(ovt_MetricsJob* job) {
  delete job;  // NOLINT(cppcoreguidelines-owning-memory): C handle
}

}  // extern "C"
