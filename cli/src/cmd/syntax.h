// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt syntax` (chapter 12 §12.2.3, WI-1.504a): the unit list and the trace text of §12.2.3.1.
// The header trees, selectors and the hex view arrive with WI-1.504b and WI-1.504c.

#ifndef OVT_CLI_CMD_SYNTAX_H
#define OVT_CLI_CMD_SYNTAX_H

#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>
#include <vector>

#include "exit_codes.h"
#include "output/envelope.h"

namespace ovt::cli {
class Cli;
}  // namespace ovt::cli

namespace ovt::cli::cmd {

struct SyntaxArgs {
  CLI::App* app = nullptr;
  std::string file;
  int track = -1;  // -1 = first video track
  bool trace = false;
};

void add_syntax_command(Cli& cli, SyntaxArgs& args);

struct SyntaxOutcome {
  ExitCode code = ExitCode::Ok;
  std::vector<Diagnostic> diagnostics;
  bool write_failed = false;
};

// Runs `syntax --trace` into `out`; the caller has checked the format (text).
[[nodiscard]] SyntaxOutcome run_syntax(const SyntaxArgs& args, std::FILE* out);

}  // namespace ovt::cli::cmd

#endif  // OVT_CLI_CMD_SYNTAX_H
