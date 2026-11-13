// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The CNT-* diagnostic registry (chapter 08 §8.5, WI-1.200): codes, severities and message templates from
// diag_codes.inc, and the lookup that `ovt probe --list-diagnostics` and the module's diagnostics use.

#ifndef OVT_CONTAINER_DIAGNOSTICS_H
#define OVT_CONTAINER_DIAGNOSTICS_H

#include <cstdint>
#include <span>
#include <string_view>

namespace ovt::container {

// Values equal ovt.fb.Severity.
enum class Severity : uint8_t { Info = 0, Warning = 1, Error = 2, Fatal = 3 };

struct DiagInfo {
  std::string_view code;  // "CNT-0101"
  Severity severity;
  std::string_view message;   // template; parameters are in backticks
  std::string_view spec_ref;  // "" when none
};

// Identifiers for use in code: DiagCode::CNT_0006. Order equals diag_codes.inc.
enum class DiagCode : uint16_t {
#define X(id, code, sev, msg, ref) id,
#include "container/diag_codes.inc"
#undef X
};

// The whole registry in diag_codes.inc order (the data source of `ovt probe --list-diagnostics`).
[[nodiscard]] std::span<const DiagInfo> all_diagnostics() noexcept;
[[nodiscard]] const DiagInfo& diag(DiagCode code) noexcept;
// nullptr for a code that is not in the registry.
[[nodiscard]] const DiagInfo* diag(std::string_view code) noexcept;

}  // namespace ovt::container

#endif  // OVT_CONTAINER_DIAGNOSTICS_H
