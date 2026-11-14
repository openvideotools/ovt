// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Canonical builder and structural FlatBuffers diff (chapter 06 §6.11 WI-0.085, rule V-032 of §6.9).
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header. The two declarations
// are the WI-0.085 Interface line plus [[nodiscard]].

#ifndef OVT_CXX_MODEL_CANONICAL_HPP
#define OVT_CXX_MODEL_CANONICAL_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/model/schema_id.hpp"

namespace ovt::model {

/// Rebuilds @p buffer (a finished, not size-prefixed buffer whose root is @p root_table, e.g.
/// "ovt.fb.FrameAnalysis") from the embedded `.bfbs` of @p schema, the way the flatc object API (`Pack`)
/// builds it: children in field-id order, then the table with its fields added largest first; scalars equal
/// to their default, empty strings and empty vectors are left out (unless `required`); struct padding is
/// zero. Vectors keep their order (§6.3.7 orders are not imposed: sorting would hide producer bugs). Nested
/// buffers (a `[ubyte]` field `X` next to a string field `X_type` naming a table) are canonicalised
/// recursively before the outer table. The schema's file_identifier is written when @p buffer carries it.
/// The output is deterministic, and canonicalize(canonicalize(b)) == canonicalize(b).
/// @throws std::invalid_argument for an unknown schema or root table, a buffer (or nested buffer) that fails
///         the reflection verifier (max_depth 64, max_tables 10 000 000), nested buffers more than 4 levels
///         deep, or a buffer whose shared offsets expand past 16 x its size + 1 MiB of work (capped below 2 GiB).
[[nodiscard]] std::vector<std::uint8_t> canonicalize(std::span<const std::uint8_t> buffer, SchemaId schema,
                                                     std::string_view root_table);

/// Structural diff behind `ovt dump --diff A B` (chapter 12 §12.2.6): walks both trees by reflection in
/// field-id order and returns one line `<path>: <a value> != <b value>` per difference, with FlatBuffers-style
/// paths as in §6.9 (`grid.qp[5]: 30 != 31`, nested buffers continue the path: `blocks[2].ext.QpY`). Empty
/// when the buffers are structurally equal (an absent field equals its default; an absent string or vector
/// equals an empty one). Enums print their value name, strings are quoted, a vector whose lengths differ
/// gets a `<path>.length` line and its common prefix is compared element by element.
/// @throws std::invalid_argument for an unknown schema or root table, a buffer that fails the verifier, nested
///         buffers more than 4 levels deep, or work past 16 x (size of A + size of B) + 1 MiB (capped below 2 GiB).
[[nodiscard]] std::vector<std::string> structural_diff(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b,
                                                       SchemaId schema, std::string_view root_table);

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_CANONICAL_HPP
