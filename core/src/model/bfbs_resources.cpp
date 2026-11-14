// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The embedded binary schemas and the schema identity accessors (chapter 06 WI-0.083).
//
// The byte arrays themselves are generated: tools/embed_bfbs.py writes ovt_schemas_bfbs.inc and CMake's
// configure_file writes model/schema_hash.inc, both below the build tree's generated/ root (chapter 13
// §13.2.3), so nothing generated lands in the source tree.
// SPEC-ASSUMPTION(#80): this file is the committed accessor and includes the generated arrays.

#include <cstdint>
#include <span>
#include <string_view>

#include "ovt/cxx/model/schema_id.hpp"
#include "ovt/cxx/model/schema_version.hpp"

#include "model/schema_hash.inc"  // kOvtModelSchemaSha256Text (configure time)
#include "ovt_schemas_bfbs.inc"   // k_<schema>_bfbs[] and OVT_BFBS_HAVE_<SCHEMA> (build time)

namespace ovt::model {

const std::string_view kModelSchemaSha256 = kOvtModelSchemaSha256Text;
const std::string_view kContainerSchemaSha256 = kOvtContainerSchemaSha256Text;
const std::string_view kMetricsSchemaSha256 = kOvtMetricsSchemaSha256Text;

std::span<const std::uint8_t> bfbs(SchemaId id) noexcept {
  switch (id) {
#ifdef OVT_BFBS_HAVE_OVT_MODEL
    case SchemaId::Model:
      return k_ovt_model_bfbs;
#endif
#ifdef OVT_BFBS_HAVE_OVT_CONTAINER
    case SchemaId::Container:
      return k_ovt_container_bfbs;
#endif
#ifdef OVT_BFBS_HAVE_OVT_METRICS
    case SchemaId::Metrics:
      return k_ovt_metrics_bfbs;
#endif
    default:
      return {};
  }
}

std::uint32_t schema_major(SchemaId id) noexcept {
  switch (id) {
    case SchemaId::Model:
      return kModelSchemaMajor;
    case SchemaId::Container:
      return kContainerSchemaMajor;
    case SchemaId::Metrics:
      return kMetricsSchemaMajor;
  }
  return 0;
}

std::string_view root_table_name(SchemaId id) noexcept {
  switch (id) {
    case SchemaId::Model:
      return "ovt.fb.FrameAnalysis";
    case SchemaId::Container:
      return "ovt.fb.ContainerRoot";
    case SchemaId::Metrics:
      return "ovt.fb.MetricsReport";
  }
  return {};
}

}  // namespace ovt::model
