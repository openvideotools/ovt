// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ovt front-end (chapter 12 §12.1, WI-1.500): the CLI11 tree, the global options of §12.1.2 resolved
// into ovt::cli::Globals (flag > environment variable > built-in default), exit codes (§12.1.3), the
// stderr logger, --progress and the SIGINT/SIGTERM handling.
//
// run() takes its streams, TTY flags and environment from a Context, so tests drive it in-process.

#ifndef OVT_CLI_APP_H
#define OVT_CLI_APP_H

#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "exit_codes.h"

namespace ovt::cli {

// --format values (§12.1.2). aom-inspect/mv-csv belong to `analyze`, vmaf-json to `metrics`.
enum class Format : std::uint8_t { Text, Json, Fb, Csv, Yuview, AomInspect, MvCsv, VmafJson };
[[nodiscard]] std::string_view format_name(Format f) noexcept;
[[nodiscard]] std::optional<Format> parse_format(std::string_view name) noexcept;  // accepts yuview-csv

enum class ColorMode : std::uint8_t { Auto, Always, Never };

// The global options of §12.1.2 after precedence resolution.
struct Globals {
  Format format = Format::Text;
  bool format_from_flag = false;  // --format/-f/--json/--text/--flatbuffers given (not env, not TTY default)
  bool pretty = false;            // JSON layout: --pretty/--compact, else by the stdout TTY test
  std::string output = "-";
  std::string cache_dir;
  bool no_cache = false;
  std::vector<std::string> plugin_dirs;
  unsigned threads = 0;  // 0 = hardware concurrency capped at 16 (effective_threads)
  int log_level = 3;     // OVT_LOG_WARN; 6 = off
  std::string log_file;
  bool partial_ok = false;
  bool progress = false;
  bool time = false;
  bool quiet = false;
  ColorMode color = ColorMode::Auto;
};

[[nodiscard]] unsigned effective_threads(const Globals& g) noexcept;

struct Context {
  std::FILE* out = stdout;
  std::FILE* err = stderr;
  bool stdout_tty = false;
  bool stderr_tty = false;
  // Environment lookup; empty = the process environment.
  std::function<std::optional<std::string>(const std::string&)> getenv;
};

// The CLI11 tree with its bound variables.
class Cli {
 public:
  Cli();
  Cli(const Cli&) = delete;
  Cli& operator=(const Cli&) = delete;
  Cli(Cli&&) = delete;
  Cli& operator=(Cli&&) = delete;
  ~Cli() = default;

  [[nodiscard]] CLI::App& app() noexcept { return app_; }

  struct Raw {
    std::string format;
    bool json = false;
    bool text = false;
    bool flatbuffers = false;
    bool pretty = false;
    bool compact = false;
    std::string output = "-";
    std::string cache_dir;
    bool no_cache = false;
    std::vector<std::string> plugin_dirs;
    unsigned threads = 0;
    std::string log_level;
    std::string log_file;
    bool partial_ok = false;
    bool progress = false;
    bool time = false;
    bool quiet = false;
    std::string color = "auto";
    bool version = false;
  };
  [[nodiscard]] const Raw& raw() const noexcept { return raw_; }
  [[nodiscard]] bool given(std::string_view long_name) const;  // the option appeared on the command line

 private:
  CLI::App app_;
  Raw raw_;
};

// Runs one invocation; `args` excludes argv[0]. Returns the process exit code.
int run(const std::vector<std::string>& args, Context& ctx);

// SPEC-ASSUMPTION(#100): commands register ovt_job_cancel through set_cancel_hook once ovt_Job exists.
// SIGINT/SIGTERM (§12.1.3): the first signal sets a flag and calls the cancel hook (a function that only
// stores an atomic, such as ovt_job_cancel); the second calls _exit(130). Never exit() in the handler.
void install_signal_handlers();
[[nodiscard]] bool cancel_requested() noexcept;
void set_cancel_hook(void (*fn)(void*), void* arg) noexcept;
void reset_cancel_state_for_testing() noexcept;

}  // namespace ovt::cli

#endif  // OVT_CLI_APP_H
