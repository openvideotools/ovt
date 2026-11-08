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

struct EnvelopeInput {
  std::string path;  // exactly as given on the command line (determinism, §12.1.4)
  std::uint64_t size_bytes = 0;
  std::vector<std::uint8_t> source_hash;  // SHA-256 bytes; JSON prints lower-case hex
};

// --time (§12.1.2): the members a command measured, in the order it adds them.
struct Timing {
  std::vector<std::pair<std::string, double>> values;  // e.g. {"index_ms", 812.5}
  std::optional<bool> cache_hit;
};

struct Envelope {
  std::string command;  // "probe", "index", …
  std::optional<EnvelopeInput> input;
  std::vector<Diagnostic> diagnostics;
  ExitCode status = ExitCode::Ok;
  std::optional<Timing> timing;
};

}  // namespace ovt::cli

#endif  // OVT_CLI_OUTPUT_ENVELOPE_H
