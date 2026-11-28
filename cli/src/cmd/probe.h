// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt probe` (chapter 12 §12.2.2, WI-1.503): the container box tree with every decoded field, plus the derived views
// of chapter 08 on request. Part a: the tree with --expand and --offset.

#ifndef OVT_CLI_CMD_PROBE_H
#define OVT_CLI_CMD_PROBE_H

#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "cmd/cache.h"  // UsageIssue
#include "exit_codes.h"
#include "output/envelope.h"

namespace ovt::cli {
class Cli;
struct Globals;
}  // namespace ovt::cli

namespace ovt::cli::cmd {

// The bound variables of the `probe` subcommand.
struct ProbeArgs {
  CLI::App* app = nullptr;
  std::string file;
  std::string expand = "all";
  bool boxes = false;
  bool tree = false;  // alias of --boxes --expand all
  std::optional<std::uint64_t> offset;
};

// Adds `probe` to cli.app().
void add_probe_command(Cli& cli, ProbeArgs& args);

// Checks the flag combination before any output is opened (exit 2 when set).
[[nodiscard]] std::optional<UsageIssue> check_probe_args(const ProbeArgs& args);

struct ProbeOutcome {
  ExitCode code = ExitCode::Ok;
  std::vector<Diagnostic> diagnostics;  // also shown on stderr by the caller for text and csv output
  std::optional<UsageIssue> usage;      // exit 2 found only after opening the file (an --offset with no box)
  bool write_failed = false;
};

// Runs the parsed subcommand, writing its result to `out`.
[[nodiscard]] ProbeOutcome run_probe(const ProbeArgs& args, const Globals& g, std::FILE* out);

}  // namespace ovt::cli::cmd

#endif  // OVT_CLI_CMD_PROBE_H
