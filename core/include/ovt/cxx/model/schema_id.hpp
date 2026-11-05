// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Schema selector and the embedded binary schemas (chapter 06 §6.11, WI-0.083). SchemaId is the selector
// that canonicalize (WI-0.085), to_json/from_json (WI-0.088) and the validator (WI-0.086) take.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header. The declarations
// are chapter 06 WI-0.083 verbatim plus [[nodiscard]] and noexcept.

#ifndef OVT_CXX_MODEL_SCHEMA_ID_HPP
#define OVT_CXX_MODEL_SCHEMA_ID_HPP

#include <cstdint>
#include <span>
#include <string_view>

namespace ovt::model {

/// One of the three FlatBuffers schemas of core/schema/.
enum class SchemaId : std::uint8_t {
  Model = 1,      ///< ovt_model.fbs, root table ovt.fb.FrameAnalysis
  Container = 2,  ///< ovt_container.fbs, root table ovt.fb.ContainerRoot
  Metrics = 3,    ///< ovt_metrics.fbs, root table ovt.fb.MetricsReport
};

/// The embedded binary schema (.bfbs, `flatc --schema --binary --bfbs-comments`), 8-byte aligned, ready for
/// reflection::GetSchema. Empty for a schema whose .fbs was not part of the build, and for an unknown id.
[[nodiscard]] std::span<const std::uint8_t> bfbs(SchemaId id) noexcept;

/// kModelSchemaMajor / kContainerSchemaMajor / kMetricsSchemaMajor (ovt/cxx/model/schema_version.hpp);
/// 0 for an unknown id.
[[nodiscard]] std::uint32_t schema_major(SchemaId id) noexcept;

/// "ovt.fb.FrameAnalysis", "ovt.fb.ContainerRoot", "ovt.fb.MetricsReport"; empty for an unknown id.
[[nodiscard]] std::string_view root_table_name(SchemaId id) noexcept;

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_SCHEMA_ID_HPP
