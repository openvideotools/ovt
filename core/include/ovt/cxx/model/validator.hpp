// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The FrameAnalysis / StreamIndex validator of chapter 06 §6.9 (rules V-001…V-032, WI-0.086). Every plugin
// buffer and every golden fixture is checked by it; `ovt analyze --validate` and `ovt index --validate`
// (chapter 12 §12.2.8 / §12.2.4) print its violations in `diagnostics[]`.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header. The C API is
// `ovt/validate.h` (chapter 12 §12.3).

#ifndef OVT_CXX_MODEL_VALIDATOR_HPP
#define OVT_CXX_MODEL_VALIDATOR_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt_model_generated.h"

namespace ovt::model {

/// One broken rule. `rule` is the rule id (`"V-012"`), `severity` Error unless the rule says otherwise (and
/// Warning for the downgraded rules of a partial record, §6.9), `message` the rule text with the offending
/// values, `path` a FlatBuffers-style locator such as `blocks[17].rect` or `grid.qp[1234]` (empty when the
/// violation concerns the whole buffer).
struct Violation {
  std::string rule;
  fb::Severity severity = fb::Severity::Error;
  std::string message;
  std::string path;
};

/// One row of the rule table that chapter 14 §14.6.1 iterates.
struct RuleInfo {
  std::string_view id;      ///< `"V-001"`
  std::string_view intent;  ///< one line: what the rule protects
  fb::Severity severity;    ///< severity of a violation in a complete (non-partial) record
};

/// At most this many violations are listed per rule; the rest are counted in one closing violation of the
/// same rule ("… and N more"), so that a hostile or badly broken buffer cannot produce millions of strings.
// SPEC-ASSUMPTION(#311): §6.9 asks for every violation; the per-rule cap bounds memory and time.
inline constexpr std::size_t kMaxViolationsPerRule = 64;

/// Validates one `FrameAnalysis` buffer (identifier `OVTA`, plain or size-prefixed) against every
/// FrameAnalysis rule of §6.9 and returns all violations (no early exit; empty = valid). When V-001's
/// Verifier check fails, nothing else is evaluated and the result is that single violation.
/// @p expected_source_hash is the stream's SHA-256 (§6.2.8); V-023 checks every `PlaneRef.blob_id` against it.
/// @p codec_override replaces the codec inferred from the block tree (§6.9 implementation note).
[[nodiscard]] std::vector<Violation> validate_frame_analysis(std::span<const std::uint8_t> buffer,
                                                             std::span<const std::uint8_t, 32> expected_source_hash,
                                                             std::optional<fb::Codec> codec_override = std::nullopt);

/// As above without a known stream hash: V-023 checks the `blob_id` grammar but not the hash it names (the
/// C API passes a null hash this way, chapter 12 §12.3).
[[nodiscard]] std::vector<Violation> validate_frame_analysis(std::span<const std::uint8_t> buffer,
                                                             std::optional<fb::Codec> codec_override = std::nullopt);

/// Rule table access (chapter 14 §14.6.1).
class Validator {
 public:
  /// The 32 rules in id order, V-001 first.
  [[nodiscard]] static std::span<const RuleInfo> rules() noexcept;
};

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_VALIDATOR_HPP
