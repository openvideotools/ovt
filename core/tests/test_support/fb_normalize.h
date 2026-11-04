// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Normalisation of test outputs before golden comparison (chapter 14 §14.1.3, §14.2.2, WI-0.550).
//
// normalize_json: canonical JSON text for golden files whose oracle is JSON.
// fb_normalize: structural check of a FlatBuffer before byte comparison. WI-0.550 provides the generic
// part (root offset and table bounds, optional file identifier); WI-0.556 adds the schema-aware
// normalisation (zeroing non-deterministic fields) once the schemas of WI-0.080 exist.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
/// verify_flatbuffer() reason. Until WI-0.556 the copy is unchanged: no schema defines
/// non-deterministic fields yet.
std::vector<uint8_t> fb_normalize(std::span<const uint8_t> buffer);

}  // namespace ovt::test
