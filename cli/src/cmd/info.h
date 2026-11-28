// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt info` (chapter 12 §12.2.1, WI-1.502): the "what is this file" answer in one screen. Container headers only.

#ifndef OVT_CLI_CMD_INFO_H
#define OVT_CLI_CMD_INFO_H

#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>
#include <vector>

#include "exit_codes.h"
#include "output/envelope.h"

namespace ovt::cli {
class Cli;
struct Globals;
}  // namespace ovt::cli

namespace ovt::cli::cmd {

// The bound variables of the `info` subcommand.
struct InfoArgs {
  CLI::App* app = nullptr;
  std::string file;
  bool full = false;
  bool all_tracks = false;
};

// Adds `info` to cli.app().
void add_info_command(Cli& cli, InfoArgs& args);

struct InfoOutcome {
  ExitCode code = ExitCode::Ok;
  std::vector<Diagnostic> diagnostics;  // also shown on stderr by the caller for text output
  bool write_failed = false;
};

// Runs the parsed subcommand, writing its result to `out`.
[[nodiscard]] InfoOutcome run_info(const InfoArgs& args, const Globals& g, std::FILE* out);

}  // namespace ovt::cli::cmd

#endif  // OVT_CLI_CMD_INFO_H
