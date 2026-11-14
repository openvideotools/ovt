// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The field-name rule of chapter 06 §6.8.1 that the JSON exporter and importer (WI-0.088) share.
// Internal header.

#ifndef OVT_CORE_MODEL_JSON_RULES_HPP
#define OVT_CORE_MODEL_JSON_RULES_HPP

#include <algorithm>
#include <array>
#include <string_view>

namespace ovt::model::detail {

/// The `[ubyte]` fields that are hashes or ids and are written as lowercase hex strings (§6.8.1, §12.1.4).
/// The rule applies by field name in every table; a field of another type with one of these names
/// (ExtractManifest.bytes, a ulong) is not affected.
inline constexpr std::array<std::string_view, 11> kHashFields = {"source_hash",
                                                                 "reference_source_hash",
                                                                 "distorted_source_hash",
                                                                 "default_kid",
                                                                 "system_id",
                                                                 "uuid",
                                                                 "iv",
                                                                 "kid",
                                                                 "kids",
                                                                 "default_constant_iv",
                                                                 "bytes"};

constexpr bool is_hash_field(std::string_view name) {
  return std::ranges::find(kHashFields, name) != kHashFields.end();
}

}  // namespace ovt::model::detail

#endif  // OVT_CORE_MODEL_JSON_RULES_HPP
