// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reflection-based JSON exporter of the three FlatBuffers schemas (chapter 06 §6.8.1 and §6.11 WI-0.088; the
// JSON of `--format json`, chapter 12 §12.1.4). C++ helper header (D22): semi-public, no ABI promise, never
// included from a C header. The declaration is the WI-0.088 Interface line plus [[nodiscard]] and a default
// for the options. The importer (from_json) is part b of the work item.

#ifndef OVT_CXX_MODEL_JSON_HPP
#define OVT_CXX_MODEL_JSON_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ovt/cxx/model/schema_id.hpp"

namespace ovt::model {

struct JsonOptions {
  /// false: 2-space indentation, one field or vector element per line, a final newline. true: one line, no
  /// newline anywhere (`"key": value` pairs separated by `,`).
  bool compact = false;
  /// true: NaN and the infinities are written as the strings "nan", "inf" and "-inf", so the text is RFC 8259
  /// JSON. false: they are written as the bare tokens nan, inf and -inf (as FlatBuffers prints them).
  bool strict_numbers = false;
};

/// The JSON text of @p buf, a finished, not size-prefixed buffer whose root is @p root_table (for example
/// "ovt.fb.FrameAnalysis") in the embedded `.bfbs` of @p schema. Field for field, in schema order (§6.8.1):
///  - every scalar field is written, also when it equals its default; an absent string, vector, struct or
///    table is left out; a present empty vector is `[]`;
///  - enums are their value names, an unknown value is its integer; bools are true/false (a stored byte
///    above 1 is its integer);
///  - integers beyond ±2^53 are decimal strings; floats and doubles use the shortest text that reads back to
///    the same value;
///  - strings are UTF-8 with the JSON escapes (control characters as \u00XX);
///  - a `[ubyte]` field is an array of numbers, except the hash-like fields (`source_hash`, `uuid`, `kid`, ...,
///    the list of §6.8.1), which are lowercase hex strings, and a nested buffer (a `[ubyte]` field `X` next to
///    a string field `X_type` that names a table), which is replaced by the JSON object of that table. When
///    `X_type` is empty or names no table the bytes stay an array of numbers.
/// The output is a deterministic function of the buffer's values.
/// @throws std::invalid_argument for an unknown schema or root table, a buffer (or a nested buffer whose
///         `X_type` names a table) that fails the reflection verifier, a string that is not UTF-8, nested
///         buffers more than 4 levels deep, or a buffer whose shared offsets expand past 16 x its size +
///         1 MiB of work (capped below 2 GiB).
[[nodiscard]] std::string to_json(std::span<const std::uint8_t> buf, SchemaId schema, std::string_view root_table,
                                  JsonOptions options = {});

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_JSON_HPP
