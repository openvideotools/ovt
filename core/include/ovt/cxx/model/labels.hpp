// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Human-readable labels for codec-numbered values (chapter 06 §6.3.1, WI-0.084). The renderer's legend
// strings, the exporters and the CLI take their labels from here, so §6.3.1 is implemented once.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_MODEL_LABELS_HPP
#define OVT_CXX_MODEL_LABELS_HPP

#include <cstdint>
#include <span>
#include <string_view>

#include "ovt_model_generated.h"

namespace ovt::model {

/// The label of @p value of @p field for @p codec, or an empty view when the value has none (a sentinel such
/// as -1 / 0xFF, a value outside the table, a codec the table does not cover, or an unknown field). The
/// returned view points to static storage.
///
/// @p field is one of two things:
/// - a §6.3.1 field (see label_fields()): "pred" (PredKind, any codec), "intra_mode_y", "intra_mode_c",
///   "tx_type", "filter_idx", and the per-kind `Block.partition_type` tables "mb_type" / "sub_mb_type"
///   (H.264), "part_mode" (HEVC), "partition" (AV1). Example: ("intra_mode_y", H265, 26) -> "INTRA_ANGULAR26".
/// - the unqualified name of any enum of ovt_model.fbs ("AvcMbType", "HevcPredMode", "Av1YMode", …); the
///   label is the enumerator name and @p codec is ignored. Example: ("Av1YMode", any, 13) -> "NEARESTMV".
// SPEC-ASSUMPTION(#98): the field names, the per-kind partition_type field names and the label strings that
// §6.3.1 does not spell out (H.264 tx_type, HEVC PCM/palette, AV1 intrabc/palette, AV1 cdef_idx).
[[nodiscard]] std::string_view label_for(std::string_view field, fb::Codec codec, std::int32_t value);

/// The §6.3.1 field names label_for knows, in a fixed order (for legends and the snapshot test).
[[nodiscard]] std::span<const std::string_view> label_fields() noexcept;

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_LABELS_HPP
