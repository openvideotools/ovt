// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Exit codes of the ovt command-line tool: the one table of chapter 12 §12.1.3 (D3).

#ifndef OVT_CLI_EXIT_CODES_H
#define OVT_CLI_EXIT_CODES_H

#include <string_view>

#include "ovt/status.h"

namespace ovt::cli {

enum class ExitCode : int {
  Ok = 0,
  Error = 1,
  Usage = 2,
  Io = 3,
  Unsupported = 4,
  Corrupt = 5,
  Plugin = 6,
  Cancelled = 7,
  Mismatch = 8,
  Partial = 9,
};

// A second SIGINT/SIGTERM exits immediately with this code (§12.1.3 "Signals").
inline constexpr int kExitSecondSignal = 130;

// The "Maps from ovt_status" column of §12.1.3. Codes 8 and 9 have no status.
[[nodiscard]] constexpr ExitCode from_status(ovt_status status) noexcept {
  switch (status) {
    case OVT_OK:
      return ExitCode::Ok;
    case OVT_E_INVALID_ARG:
      return ExitCode::Usage;
    case OVT_E_IO:
      return ExitCode::Io;
    case OVT_E_UNSUPPORTED:
      return ExitCode::Unsupported;
    case OVT_E_CORRUPT:
      return ExitCode::Corrupt;
    case OVT_E_CANCELLED:
      return ExitCode::Cancelled;
    case OVT_E_PLUGIN:
      return ExitCode::Plugin;
    case OVT_E_NOMEM:
    case OVT_E_INTERNAL:
      break;
  }
  return ExitCode::Error;  // OVT_E_INTERNAL, OVT_E_NOMEM and any value a newer library adds
}

// The "Name" column; the envelope's `status` member (§12.1.4).
[[nodiscard]] constexpr std::string_view exit_code_name(ExitCode code) noexcept {
  switch (code) {
    case ExitCode::Ok:
      return "OK";
    case ExitCode::Error:
      return "ERROR";
    case ExitCode::Usage:
      return "USAGE";
    case ExitCode::Io:
      return "IO";
    case ExitCode::Unsupported:
      return "UNSUPPORTED";
    case ExitCode::Corrupt:
      return "CORRUPT";
    case ExitCode::Plugin:
      return "PLUGIN";
    case ExitCode::Cancelled:
      return "CANCELLED";
    case ExitCode::Mismatch:
      return "MISMATCH";
    case ExitCode::Partial:
      return "PARTIAL";
  }
  return "ERROR";
}

}  // namespace ovt::cli

#endif  // OVT_CLI_EXIT_CODES_H
