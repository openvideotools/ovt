// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Normalisation of test outputs before golden comparison (chapter 14 §14.1.3, §14.2.2, WI-0.550).
//
// normalize_json: canonical JSON text for golden files whose oracle is JSON.
// fb_normalize: structural check of a FlatBuffer before byte comparison. WI-0.550 provides the generic
// part (root offset and table bounds, optional file identifier). WI-0.556 did not add byte-level zeroing:
// the analysis-record schemas of ch.6 define no non-deterministic field (§0.7.5), so fb_normalize() stays the
// identity after verification.
//
// WI-0.556 adds to_canonical(): the family writers of §14.4.0 that turn OVT's FlatBuffer tables into the
// canonical JSON families (syntax-elements, header-struct, block-grid, box-tree, sample-table, scores), the
// C++ twin of tests/oracles/normalize/*.py. Numbers are written as integers or doubles; the golden writer
// (normalize_json) then prints doubles with %.6f, the same path as the Python normalisers.

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ovt::fb {
struct StreamIndex;
struct SyntaxNode;
struct ContainerTree;
struct SampleTable;
struct FrameAnalysis;
struct MetricsResult;
}  // namespace ovt::fb

namespace ovt::test {

/// Parses @p json and re-serialises it canonically: object keys sorted (byte order), 2-space
/// indentation, integers unchanged, every non-integer number with exactly 6 decimals, a final newline.
/// Throws std::invalid_argument on malformed input.
std::string normalize_json(std::string_view json);

/// Checks that @p buffer is a structurally valid FlatBuffer whose root table lies inside the buffer
/// (flatbuffers::Verifier on the root table header), and, when @p file_identifier is given, that the
/// buffer carries that 4-character identifier. Returns std::nullopt when valid, otherwise the reason.
std::optional<std::string> verify_flatbuffer(std::span<const uint8_t> buffer,
                                             std::optional<std::string_view> file_identifier = std::nullopt);

/// Returns a copy of @p buffer ready for byte comparison, or throws std::invalid_argument with the
/// verify_flatbuffer() reason. The copy is unchanged: no analysis-record schema defines a
/// non-deterministic field (WI-0.556).
std::vector<uint8_t> fb_normalize(std::span<const uint8_t> buffer);

/// The canonical JSON families of chapter 14 §14.4.0.
enum class Family : std::uint8_t { kSyntaxElements, kHeaderStruct, kBlockGrid, kBoxTree, kSampleTable, kScores };

/// "syntax-elements", "header-struct", "block-grid", "box-tree", "sample-table", "scores".
std::string_view family_name(Family family);
/// Inverse of family_name(); std::nullopt for an unknown name.
std::optional<Family> family_from_name(std::string_view name);

/// syntax-elements: `[{"unit","name","bit_offset","bit_length","value"}]` sorted by (unit, bit_offset). Walks
/// every Unit.header of @p index depth first; nodes without a value (kind None) are containers and are not
/// listed. Value: UInt -> unsigned, Int/Fixed -> signed, Bool -> 0/1, Enum -> enum_name when set, else the
/// integer, String -> string, Bytes -> lower-case hex string.
// SPEC-ASSUMPTION(#111): ch.6 has no SyntaxTree table; the input is StreamIndex (or one SyntaxNode below).
nlohmann::json to_canonical(const fb::StreamIndex& index, Family family);

/// syntax-elements / header-struct of one header tree that belongs to unit @p unit_index.
nlohmann::json to_canonical(const fb::SyntaxNode& header, Family family, uint32_t unit_index = 0);

/// box-tree: `[{"path","offset","size","fields"}]` in file order, depth first. path is the slash-joined fourcc
/// chain; a fourcc that occurs more than once among its siblings gets a 1-based `[n]` suffix
/// ("moov/trak[1]/mdia"). fields: Field.name -> value; a Field with children becomes an object (named
/// children) or an array (unnamed children).
nlohmann::json to_canonical(const fb::ContainerTree& tree, Family family = Family::kBoxTree);

/// sample-table: `[{"i","dts","cts","size","offset","sync"}]`; cts = dts + cts_offset; sync = 0|1.
nlohmann::json to_canonical(const fb::SampleTable& table, Family family = Family::kSampleTable);

/// block-grid: `{"w4","h4","fields":{name:[int per cell]}}` from FrameAnalysis.grid. @p fields selects CellGrid
/// vectors by name (empty = all vectors the grid carries, except block_id/partition_id). Unknown name:
/// std::invalid_argument.
nlohmann::json to_canonical(const fb::FrameAnalysis& analysis, Family family = Family::kBlockGrid,
                            const std::vector<std::string>& fields = {});

/// scores: `{"frames":[{"i":n,"<metric>":v}],"pooled":{"<metric>":{"mean","harmonic_mean","min"}}}`. A result
/// with one value per frame is keyed by the metric id; with several, by "<id>_y", "<id>_u", "<id>_v"
/// (more than three: "<id>_<n>").
nlohmann::json to_canonical(const fb::MetricsResult& result, Family family = Family::kScores);

}  // namespace ovt::test
