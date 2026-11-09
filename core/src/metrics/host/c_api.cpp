// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/metrics.h entry points (chapter 9 §9.10, chapter 12 §12.3.13; WI-3.300b: ovt_metrics_list_descriptors;
// WI-3.300c adds the ovt_metrics_job_* calls, WI-3.316 the map and plane calls). No exception leaves these functions
// (§12.3.1 rule 8): every body runs inside a catch-all that maps std::bad_alloc to OVT_E_NOMEM and anything else to
// OVT_E_INTERNAL. The C declarations carry no exception specification, so the definitions cannot spell noexcept either
// (same as jobs_api.cpp).

#include <exception>
#include <new>
#include <string>

#include "api/buffer.hpp"
#include "metrics/host/plugin_registry.h"
#include "ovt/cxx/log.hpp"
#include "ovt/metrics.h"

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

}  // extern "C"
