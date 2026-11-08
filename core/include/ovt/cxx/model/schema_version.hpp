// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Compile-time schema majors (chapter 06 §6.1, WI-0.083). A major changes only on a breaking schema change
// (§6.1 evolution rules); the cache index check I3 of §6.7 compares meta.json against these values.

#ifndef OVT_CXX_MODEL_SCHEMA_VERSION_HPP
#define OVT_CXX_MODEL_SCHEMA_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace ovt::model {

inline constexpr std::uint32_t kModelSchemaMajor = 1;      ///< ovt_model.fbs
inline constexpr std::uint32_t kContainerSchemaMajor = 1;  ///< ovt_container.fbs
inline constexpr std::uint32_t kMetricsSchemaMajor = 1;    ///< ovt_metrics.fbs

/// SHA-256 (64 lower-case hex digits) of core/schema/ovt_model.fbs as tools/schema_hash.py computes it (LF
/// line endings), recorded at configure time. It is the `schemas.ovt_model.sha256` of the cache meta.json (§6.7).
// SPEC-ASSUMPTION(#80): defined in libovt-core from a configure-time file under the build tree's
// generated/ root, so this header stays a plain source file that installs unchanged.
extern const std::string_view kModelSchemaSha256;
/// The same for ovt_container.fbs and ovt_metrics.fbs (`schemas.ovt_container.sha256`, `schemas.ovt_metrics.sha256`;
/// WI-0.091).
extern const std::string_view kContainerSchemaSha256;
extern const std::string_view kMetricsSchemaSha256;

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_SCHEMA_VERSION_HPP
