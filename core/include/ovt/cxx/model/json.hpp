// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reflection-based JSON exporter and importer of the three FlatBuffers schemas (chapter 06 §6.8.1 and §6.11
// WI-0.088; the JSON of `--format json`, chapter 12 §12.1.4). C++ helper header (D22): semi-public, no ABI
// promise, never included from a C header. The declarations are the WI-0.088 Interface line plus
// [[nodiscard]] and a default for the options.

#ifndef OVT_CXX_MODEL_JSON_HPP
#define OVT_CXX_MODEL_JSON_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/model/schema_id.hpp"

namespace ovt::model {

struct JsonOptions {
  /// false: 2-space indentation, one field or vector element per line, a final newline. true: one line, no
  /// newline anywhere (`"key": value` pairs separated by `,`).
  bool compact = false;
  /// true: NaN and the infinities are written as the strings "nan", "inf" and "-inf", so the text is RFC 8259
  /// JSON. false: they are written as the bare tokens nan, inf and -inf (as FlatBuffers prints them).
  /// The CLI always passes true (chapter 12 §12.1.4); false is for library callers that want the §6.8.1 text.
  bool strict_numbers = false;
};

/// The JSON text of @p buf, a finished, not size-prefixed buffer whose root is @p root_table (for example
/// "ovt.fb.FrameAnalysis") in the embedded `.bfbs` of @p schema. Field for field, in schema order (§6.8.1):
///  - every scalar field is written, also when it equals its default; an absent string, vector, struct or
///    table is left out; a present empty vector is `[]`;
///  - enums are their value names, an unknown value is its integer; bools are true/false (a stored byte
///    above 1 is its integer);
///  - integers beyond ±2^53 are decimal strings; floats and doubles use the shortest digits that read back to
///    the same value, laid out as ECMAScript does (plain up to 21 integer digits and down to 1e-6, `1.5e+21`
///    and `1e-7` beyond);
///  - strings are UTF-8 with the JSON escapes (control characters as \u00XX);
///  - a `[ubyte]` field is an array of numbers, except the hash-like fields (`source_hash`, `uuid`, `kid`, ...,
///    the list of §6.8.1), which are lowercase hex strings, and a nested buffer, which is replaced by the JSON
///    object of its table. A nested buffer is a non-empty `[ubyte]` field `X` next to a string field `X_type`
///    that names a table (`Block.ext`, `FrameAnalysis.filter_params`, `FrameInfo.ref_state`), or one of the
///    two fields whose table the schema fixes: `HevcCu.palette` (an `HevcPalette`) and `Av1Block.palette` (an
///    `Av1Palette`). When `X_type` is empty or names no table the bytes stay an array of numbers.
/// The output is a deterministic function of the buffer's values.
/// @throws std::invalid_argument for an unknown schema or root table, a buffer (or a nested buffer whose
///         `X_type` names a table, or a palette) that fails the reflection verifier, a string that is not UTF-8, nested
///         buffers more than 4 levels deep, or a buffer whose shared offsets expand past 16 x its size +
///         1 MiB of work (capped below 2 GiB).
[[nodiscard]] std::string to_json(std::span<const std::uint8_t> buf, SchemaId schema, std::string_view root_table,
                                  JsonOptions options = {});

/// The reverse of to_json: parses @p json (either layout, either number style) as @p root_table and returns
/// the canonical buffer (canonicalize(), WI-0.085), so that from_json(to_json(b)) == canonicalize(b) for a
/// buffer without NaN payloads. The schema's file identifier is written when @p root_table is the schema's
/// root type (root_table_name()), none otherwise. An object under a nested-buffer field is parsed as the
/// table its `X_type` sibling names (for a palette field, its fixed table) and embedded as bytes; an array of numbers
/// there is taken as the bytes. A hash-like field accepts a hex string or an array of numbers. Numbers may be quoted.
/// Unknown keys are ignored (§12.1.4 compatibility policy); a missing key leaves the field at its default.
/// @throws std::invalid_argument for an unknown schema or root table, and for text the FlatBuffers JSON
///         parser rejects (the message carries its line and column), an odd or non-hex hash string, an object
///         under a nested-buffer field whose `X_type` names no table, or nesting deeper than 64 levels.
[[nodiscard]] std::vector<std::uint8_t> from_json(std::string_view json, SchemaId schema, std::string_view root_table);

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_JSON_HPP
