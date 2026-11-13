// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// NormalizationConfig JSON: validation against core/schema/normalization_config.schema.json, default
// expansion and the canonical serialisation of chapter 06 rule V-030 (chapter 9 §9.2.4; WI-3.300 step 5).
// Internal header of the metrics module (not installed).
//
// The resolved form is what MetricsResult.normalization carries: every schema property that has a resolved
// value, defaults expanded, keys sorted lexicographically at every level, no whitespace, numbers in their
// shortest round-trip form. Properties without a default (colour.primaries/transfer/matrix,
// display_model.peak_nits/black_nits/hlg_system_gamma) appear only when the caller set them; the nullable
// crop rectangles appear as null when not set. display_model.preset stays "auto" here: it resolves against
// the probed stream (WI-3.303), as temporal.offset_frames and spatial.shift_* resolve by alignment (WI-3.304).
// SPEC-ASSUMPTION(#290): the treatment of default-less properties in the resolved form.

#ifndef OVT_SRC_METRICS_NORMALIZE_NORMALIZATION_CONFIG_H
#define OVT_SRC_METRICS_NORMALIZE_NORMALIZATION_CONFIG_H

#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "ovt/cxx/result.hpp"

namespace ovt::metrics {

// The schema text embedded at build time from core/schema/normalization_config.schema.json.
[[nodiscard]] std::string_view normalization_config_schema_text() noexcept;

// Parses `text` (a NormalizationConfig object; "" and "{}" mean defaults), validates it against the schema and
// returns the resolved configuration as a nlohmann::json object (defaults expanded, unknown keys rejected).
// Errors are OVT_E_INVALID_ARG; the message names the offending property in dotted form (`scale.kernel`).
[[nodiscard]] Result<nlohmann::json> resolve_normalization_config(std::string_view text);

// The canonical string of a resolved configuration (rule V-030): sorted keys, no whitespace.
[[nodiscard]] std::string canonical_normalization_json(const nlohmann::json& resolved);

// resolve + canonical in one call: what MetricsResult.normalization stores.
[[nodiscard]] Result<std::string> resolve_normalization_config_json(std::string_view text);

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_NORMALIZE_NORMALIZATION_CONFIG_H
