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
#include <map>
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

struct CompletionScripts {
  std::string_view bash;
  std::string_view zsh;
  std::string_view fish;
};

struct Context {
  std::FILE* out = stdout;
  std::FILE* err = stderr;
  bool stdout_tty = false;
  bool stderr_tty = false;
  // Environment lookup; empty = the process environment.
  std::function<std::optional<std::string>(const std::string&)> getenv;
  CompletionScripts completions;
};

// The CLI11 tree with its bound variables. gen_completions (cli/tools/) walks app() at build time.
class Cli {
 public:
  Cli();
  Cli(const Cli&) = delete;
  Cli& operator=(const Cli&) = delete;
  Cli(Cli&&) = delete;
  Cli& operator=(Cli&&) = delete;
  ~Cli() = default;

  [[nodiscard]] CLI::App& app() noexcept { return app_; }
  // Values of an option restricted by CLI::IsMember (canonical spellings only), for completion.
  [[nodiscard]] const std::vector<std::string>* choices(const CLI::Option* opt) const;

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
    std::string shell;  // `completion <shell>`
  };
  [[nodiscard]] const Raw& raw() const noexcept { return raw_; }
  [[nodiscard]] bool given(std::string_view long_name) const;  // the option appeared on the command line
  [[nodiscard]] CLI::App* completion() const noexcept { return completion_; }

 private:
  CLI::Option* add_choice(CLI::App& app, const std::string& name, std::string& var, const std::string& help,
                          std::vector<std::string> canonical, std::vector<std::string> extra = {});

  CLI::App app_;
  Raw raw_;
  CLI::App* completion_ = nullptr;
  std::map<const CLI::Option*, std::vector<std::string>> choices_;
};

// Runs one invocation; `args` excludes argv[0]. Returns the process exit code.
int run(const std::vector<std::string>& args, Context& ctx);

// #100 (accepted): commands register ovt_job_cancel through set_cancel_hook once ovt_Job exists.
// SIGINT/SIGTERM (§12.1.3): the first signal sets a flag and calls the cancel hook (a function that only
// stores an atomic, such as ovt_job_cancel); the second calls _exit(130). Never exit() in the handler.
void install_signal_handlers();
[[nodiscard]] bool cancel_requested() noexcept;
void set_cancel_hook(void (*fn)(void*), void* arg) noexcept;
void reset_cancel_state_for_testing() noexcept;

// #100 (accepted): the first job-running command feeds this from ovt_job_set_progress_callback.
// --progress renderer (§12.1.2). On a TTY: one rewriting line "[####      ] 42% frames 1200/2850 ETA 00:12".
// Otherwise one JSON Lines record per >= 1 % step: {"progress":0.42,"done":1200,"total":2850,"stage":"index"}.
class ProgressReporter {
 public:
  using Clock = std::function<double()>;  // monotonic seconds; empty = std::chrono::steady_clock
  ProgressReporter(std::FILE* err, bool tty, std::string stage, std::string unit, Clock clock = {});
  void update(double fraction, std::uint64_t done, std::uint64_t total);
  void finish();  // ends the TTY line

 private:
  std::FILE* err_;
  bool tty_;
  std::string stage_;
  std::string unit_;
  Clock clock_;
  double start_;
  int last_percent_ = -1;
};

}  // namespace ovt::cli

#endif  // OVT_CLI_APP_H
