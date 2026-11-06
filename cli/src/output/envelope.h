// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The result envelope shared by the json and fb writers (chapter 12 §12.1.4; table CliEnvelope of
// chapter 06 §6.3). Plain data: the writers decide the encoding.

#ifndef OVT_CLI_OUTPUT_ENVELOPE_H
#define OVT_CLI_OUTPUT_ENVELOPE_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "exit_codes.h"

namespace ovt::cli {

// One entry of `diagnostics[]` (table Diagnostic). `severity` is the Severity enum name.
struct Diagnostic {
  std::string severity;  // "Info", "Warning", "Error", "Fatal"
  std::string code;      // D2 code, e.g. "CLI-0004"
  std::string message;
  std::optional<std::pair<std::uint64_t, std::uint32_t>> span;  // {bit_offset, bit_length}
  std::string spec_ref;                                         // omitted when empty
};

}  // namespace ovt::cli

#endif  // OVT_CLI_OUTPUT_ENVELOPE_H
