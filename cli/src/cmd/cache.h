// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt cache` (chapter 12 §12.2.13, WI-1.509): list, stats, clear, path, import.

#ifndef OVT_CLI_CMD_CACHE_H
#define OVT_CLI_CMD_CACHE_H

#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "cmd/record_io.h"
#include "exit_codes.h"
#include "output/envelope.h"

namespace ovt::cli {
class Cli;
struct Globals;
}  // namespace ovt::cli

namespace ovt::cli::cmd {

// The bound variables of the `cache` subtree.
struct CacheArgs {
  CLI::App* app = nullptr;  // `cache`
  CLI::App* list = nullptr;
  CLI::App* stats = nullptr;
  CLI::App* clear = nullptr;
  CLI::App* path = nullptr;
  CLI::App* import = nullptr;
  std::string kind;
  int older_than_days = 0;
  std::string source;
  bool yes = false;
  double max_size_gib = 0;
  std::string import_path;
  std::string source_hash;  // import: 64 hex
};

// Adds `cache` and its subcommands to cli.app().
void add_cache_command(Cli& cli, CacheArgs& args);

struct CacheEnv {
  std::FILE* in = nullptr;
  std::FILE* err = nullptr;
  bool stdin_tty = false;
  bool stderr_tty = false;
  bool color = false;
  const RecordIo* io = nullptr;  // null = unavailable
};

struct UsageIssue {
  std::string code;
  std::string message;
  std::string command;
};

struct CacheOutcome {
  ExitCode code = ExitCode::Ok;
  std::vector<Diagnostic> diagnostics;  // also shown on stderr by the caller when not part of the output
  std::optional<UsageIssue> usage;      // exit 2: the caller prints the one-line usage error
  bool write_failed = false;
};

// Runs the parsed subcommand, writing its result to `out`.
[[nodiscard]] CacheOutcome run_cache(const CacheArgs& args, const Globals& g, const CacheEnv& env, std::FILE* out);

}  // namespace ovt::cli::cmd

#endif  // OVT_CLI_CMD_CACHE_H
