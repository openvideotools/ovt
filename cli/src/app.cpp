// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <tuple>
#include <utility>

#include "output/json_writer.h"
#include "ovt/log.h"
#include "ovt/version.h"
#include "ovt_version_full.h"  // generated: OVT_VERSION_FULL = <VERSION>+<git sha>

namespace ovt::cli {

namespace {

constexpr std::string_view kName = OVT_CLI_NAME_VALUE;
constexpr unsigned kMaxDefaultThreads = 16;

constexpr std::array<std::string_view, 7> kLevelNames = {"trace", "debug", "info", "warn", "error", "fatal", "off"};

struct UsageError {
  std::string code;     // CLI-* code; empty for the messages SPEC-ASSUMPTION(#101) leaves without one
  std::string message;  // without the trailing "See '...'." part
  std::string command;  // "ovt" or "ovt <sub>"
};

std::optional<std::string> env_value(const Context& ctx, const std::string& name) {
  if (ctx.getenv) {
    return ctx.getenv(name);
  }
  const char* v = std::getenv(name.c_str());  // NOLINT(concurrency-mt-unsafe): read before any thread starts
  return v != nullptr ? std::optional<std::string>(v) : std::nullopt;
}

std::optional<int> parse_level(std::string_view name) {
  const auto* it = std::find(kLevelNames.begin(), kLevelNames.end(), name);
  return it == kLevelNames.end() ? std::nullopt : std::optional<int>(static_cast<int>(it - kLevelNames.begin()));
}

std::optional<unsigned> parse_unsigned(std::string_view s) {
  unsigned long long v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9' || v > 100000000ULL) {
      return std::nullopt;
    }
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  return s.empty() ? std::nullopt : std::optional<unsigned>(static_cast<unsigned>(v));
}

std::string default_cache_dir(const Context& ctx) {
  const std::string home = env_value(ctx, "HOME").value_or("");
#if defined(__APPLE__)
  return home + "/Library/Caches/org.openvideotools.ovt/";
#else
  const std::string xdg = env_value(ctx, "XDG_CACHE_HOME").value_or("");
  return xdg.empty() ? home + "/.cache/ovt/" : xdg + "/ovt/";
#endif
}

std::size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) {
    row[j] = j;
  }
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diag = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t up = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0U : 1U)});
      diag = up;
    }
  }
  return row[b.size()];
}

// The closest long option or subcommand name within edit distance 2, for the CLI-0001 hint.
std::string closest_spelling(const CLI::App& app, std::string_view word) {
  std::vector<std::string> names;
  for (const CLI::App* a : {&app, app.get_parent()}) {
    if (a == nullptr) {
      continue;
    }
    for (const CLI::Option* opt : a->get_options()) {
      for (const std::string& l : opt->get_lnames()) {
        names.push_back("--" + l);
      }
    }
    for (const CLI::App* sub : a->get_subcommands({})) {
      names.push_back(sub->get_name());
    }
  }
  std::string best;
  std::size_t best_d = 3;
  for (const std::string& n : names) {
    const std::size_t d = edit_distance(word, n);
    if (d < best_d) {
      best_d = d;
      best = n;
    }
  }
  return best;
}

// ---- Logging (step 8) ------------------------------------------------------------------------------------

struct LogTarget {
  std::FILE* file = nullptr;
  bool json = false;
  bool color = false;
  int level = OVT_LOG_WARN;
};

void write_log_line(const LogTarget& t, int level, std::string_view module, std::string_view msg) noexcept {
  try {
    const std::string_view name = kLevelNames.at(static_cast<std::size_t>(std::clamp(level, 0, 5)));
    std::string line;
    if (t.json) {
      line = fmt::format("{{\"level\":{},\"module\":{},\"msg\":{}}}\n", json_quote(name), json_quote(module),
                         json_quote(msg));
    } else {
      const auto now = std::chrono::system_clock::now();
      const std::time_t secs = std::chrono::system_clock::to_time_t(now);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
      std::tm tm{};
      static_cast<void>(gmtime_r(&secs, &tm));  // cannot fail for a time_t from system_clock
      std::string upper(name);
      std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return static_cast<char>(c - 32); });
      const char* on =
          !t.color ? "" : (level >= OVT_LOG_ERROR ? "\x1b[31m" : (level == OVT_LOG_WARN ? "\x1b[33m" : ""));
      line = fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z {}{:<5}{} {} {}\n", tm.tm_year + 1900,
                         tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms, on, upper,
                         *on != '\0' ? "\x1b[0m" : "", module, msg);
    }
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), t.file));
    static_cast<void>(std::fflush(t.file));
  } catch (...) {  // NOLINT(bugprone-empty-catch): a log line that cannot be formatted is dropped
  }
}

void library_log(int level, const char* module, const char* msg, void* user) noexcept {
  write_log_line(*static_cast<const LogTarget*>(user), level, module != nullptr ? module : "",
                 msg != nullptr ? msg : "");
}

// ---- Option resolution (step 2) --------------------------------------------------------------------------

struct Resolved {
  std::optional<Globals> globals;
  std::optional<UsageError> error;
};

std::optional<UsageError> resolve_format(const Cli& cli, const Context& ctx, Globals& g) {
  const Cli::Raw& r = cli.raw();
  std::vector<std::pair<std::string, Format>> chosen;  // (spelling, format) from flags
  if (cli.given("format")) {
    chosen.emplace_back("--format " + r.format, parse_format(r.format).value_or(Format::Json));  // IsMember-checked
  }
  for (const auto& [on, flag, f] :
       {std::tuple{r.json, "--json", Format::Json}, std::tuple{r.text, "--text", Format::Text},
        std::tuple{r.flatbuffers, "--flatbuffers", Format::Fb}}) {
    if (on) {
      chosen.emplace_back(flag, f);
    }
  }
  for (const auto& c : chosen) {
    if (c.second != chosen.front().second) {
      return UsageError{"CLI-0006",
                        fmt::format("options {} and {} are mutually exclusive", chosen.front().first, c.first),
                        std::string(kName)};
    }
  }
  if (!chosen.empty()) {
    g.format = chosen.front().second;
    g.format_from_flag = true;
  } else if (const auto env = env_value(ctx, "OVT_FORMAT"); env && !env->empty()) {
    const auto f = parse_format(*env);
    if (!f) {  // SPEC-ASSUMPTION(#101): a bad environment value is a usage error without a code
      return UsageError{"", fmt::format("invalid value '{}' in OVT_FORMAT", *env), std::string(kName)};
    }
    g.format = *f;
  } else {
    g.format = ctx.stdout_tty ? Format::Text : Format::Json;
  }
  if (r.pretty && r.compact) {
    return UsageError{"CLI-0006", "options --pretty and --compact are mutually exclusive", std::string(kName)};
  }
  g.pretty = r.pretty || (!r.compact && ctx.stdout_tty);
  return std::nullopt;
}

void resolve_paths(const Cli& cli, const Context& ctx, Globals& g) {
  const Cli::Raw& r = cli.raw();
  g.cache_dir = cli.given("cache-dir") ? r.cache_dir : env_value(ctx, "OVT_CACHE_DIR").value_or("");
  if (g.cache_dir.empty()) {
    g.cache_dir = default_cache_dir(ctx);
  }
  g.no_cache = r.no_cache;
  if (cli.given("plugin-dir")) {  // SPEC-ASSUMPTION(#101): the flag replaces OVT_PLUGIN_PATH
    g.plugin_dirs = r.plugin_dirs;
  } else if (const auto env = env_value(ctx, "OVT_PLUGIN_PATH")) {
    std::string_view rest(*env);
    while (!rest.empty()) {
      const std::size_t colon = std::min(rest.find(':'), rest.size());
      if (colon > 0) {
        g.plugin_dirs.emplace_back(rest.substr(0, colon));
      }
      rest.remove_prefix(std::min(colon + 1, rest.size()));
    }
  }
}

Resolved resolve(const Cli& cli, const Context& ctx) {
  const Cli::Raw& r = cli.raw();
  Globals g;
  if (auto err = resolve_format(cli, ctx, g)) {
    return {std::nullopt, std::move(err)};
  }
  g.output = r.output;
  resolve_paths(cli, ctx, g);
  if (cli.given("threads")) {
    g.threads = r.threads;
  } else if (const auto env = env_value(ctx, "OVT_THREADS"); env && !env->empty()) {
    const auto v = parse_unsigned(*env);
    if (!v) {
      return {std::nullopt, UsageError{"", fmt::format("invalid value '{}' in OVT_THREADS", *env), std::string(kName)}};
    }
    g.threads = *v;
  }
  if (cli.given("log-level")) {
    g.log_level = parse_level(r.log_level).value_or(OVT_LOG_WARN);  // IsMember-checked
  } else if (r.quiet) {  // SPEC-ASSUMPTION(#101): --log-level > --quiet > OVT_LOG_LEVEL
    g.log_level = OVT_LOG_ERROR;
  } else if (const auto env = env_value(ctx, "OVT_LOG_LEVEL"); env && !env->empty()) {
    const auto level = parse_level(*env);
    if (!level) {
      return {std::nullopt,
              UsageError{"", fmt::format("invalid value '{}' in OVT_LOG_LEVEL", *env), std::string(kName)}};
    }
    g.log_level = *level;
  }
  g.log_file = r.log_file;
  g.partial_ok = r.partial_ok;
  g.progress = r.progress;
  g.time = r.time;
  g.quiet = r.quiet;
  g.color = r.color == "always" ? ColorMode::Always : (r.color == "never" ? ColorMode::Never : ColorMode::Auto);
  return {std::move(g), std::nullopt};
}

// SPEC-ASSUMPTION(#101): exit-2 messages are always one human line, even under --format json.
void print_usage_error(const Context& ctx, const UsageError& e) {
  const std::string code = e.code.empty() ? "" : e.code + " ";
  const std::string line = fmt::format("error: {}{}. See '{} --help'.\n", code, e.message, e.command);
  static_cast<void>(std::fwrite(line.data(), 1, line.size(), ctx.err));
}

// A non-usage CLI diagnostic on stderr: JSON Lines with --format json, otherwise "error: <code> <message>".
void report(const Context& ctx, const Globals& g, const Diagnostic& d) {
  if (g.format == Format::Json) {
    JsonWriter w(ctx.err, false);
    write_diagnostic(w, d);
    static_cast<void>(w.finish());
  } else {
    std::string severity = d.severity;
    std::transform(severity.begin(), severity.end(), severity.begin(), [](char c) { return std::tolower(c); });
    const std::string line = fmt::format("{}: {} {}\n", severity, d.code, d.message);
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), ctx.err));
  }
}

std::optional<UsageError> check_format(const Globals& g, std::string_view command,
                                       std::initializer_list<Format> supported) {
  if (std::find(supported.begin(), supported.end(), g.format) != supported.end()) {
    return std::nullopt;
  }
  std::vector<std::string_view> names;
  for (const Format f : supported) {
    names.push_back(format_name(f));
  }
  std::sort(names.begin(), names.end());
  return UsageError{"CLI-0002",
                    fmt::format("format '{}' is not supported by '{}'; supported: {}", format_name(g.format), command,
                                fmt::join(names, ", ")),
                    std::string(kName) + (command == "--version" ? "" : " " + std::string(command))};
}

// ---- Output target: -o handling and partial-file removal (§12.1.2 --partial-ok) -------------------------

class OutputTarget {
 public:
  OutputTarget(const Context& ctx, const Globals& g) : g_(g), stream_(ctx.out) {}
  OutputTarget(const OutputTarget&) = delete;
  OutputTarget& operator=(const OutputTarget&) = delete;
  OutputTarget(OutputTarget&&) = delete;
  OutputTarget& operator=(OutputTarget&&) = delete;
  ~OutputTarget() {
    if (owned_) {
      static_cast<void>(std::fclose(stream_));
    }
  }

  // nullopt on success, else CLI-0004.
  std::optional<Diagnostic> open() {
    if (g_.output == "-") {
      return std::nullopt;
    }
    stream_ = std::fopen(g_.output.c_str(), "wb");
    if (stream_ == nullptr) {
      return cannot_write();
    }
    owned_ = true;
    return std::nullopt;
  }
  [[nodiscard]] std::FILE* stream() const noexcept { return stream_; }

  // Closes the file; removes it on exit 5/7 unless --partial-ok. Write errors become CLI-0004.
  std::optional<Diagnostic> close(ExitCode code) {
    bool ok = std::fflush(stream_) == 0;
    if (owned_) {
      ok = std::fclose(stream_) == 0 && ok;
      owned_ = false;
      if ((code == ExitCode::Corrupt || code == ExitCode::Cancelled) && !g_.partial_ok) {
        static_cast<void>(std::remove(g_.output.c_str()));
      }
    }
    if (ok) {
      return std::nullopt;
    }
    return cannot_write();
  }

 private:
  [[nodiscard]] Diagnostic cannot_write() const {
    const char* why = std::strerror(errno);  // NOLINT(concurrency-mt-unsafe): single-threaded at this point
    return Diagnostic{"Error", "CLI-0004", fmt::format("cannot write '{}': {}", g_.output, why), std::nullopt, ""};
  }

  const Globals& g_;
  std::FILE* stream_;
  bool owned_ = false;
};

// ---- Commands -------------------------------------------------------------------------------------------

bool write_version(const Globals& g, std::FILE* out) {
  if (g.format == Format::Text) {
    const std::string line =
        fmt::format("{} {} (core {}, api {}.{}.{})\n", kName, OVT_VERSION_FULL, ovt_version_string(),
                    OVT_API_VERSION_MAJOR, OVT_API_VERSION_MINOR, OVT_API_VERSION_PATCH);
    return std::fwrite(line.data(), 1, line.size(), out) == line.size();
  }
  const auto build = nlohmann::json::parse(ovt_build_info_json(), nullptr, false);
  const auto member = [&](const char* key) {
    return build.is_object() && build.contains(key) && build[key].is_string() ? build[key].get<std::string>() : "";
  };
  JsonWriter w(out, g.pretty);
  w.begin_object();
  w.key("ovt");
  w.string(OVT_CLI_VERSION_VALUE);
  w.key("core");
  w.string(ovt_version_string());
  w.key("api");
  w.string(fmt::format("{}.{}.{}", OVT_API_VERSION_MAJOR, OVT_API_VERSION_MINOR, OVT_API_VERSION_PATCH));
  w.key("build");
  w.begin_object();
  for (const char* key : {"commit", "profile", "platform"}) {
    w.key(key);
    w.string(member(key));
  }
  w.end_object();
  w.key("features");
  w.begin_array();
  if (build.is_object() && build.contains("features") && build["features"].is_array()) {
    for (const auto& f : build["features"]) {
      if (f.is_string()) {
        w.string(f.get<std::string>());
      }
    }
  }
  w.end_array();
  w.end_object();
  return w.finish();
}

std::atomic<int> g_signal_count{0};
std::atomic<void (*)(void*)> g_cancel_fn{nullptr};
std::atomic<void*> g_cancel_arg{nullptr};

extern "C" void ovt_cli_on_signal(int /*signo*/) {
  if (g_signal_count.fetch_add(1) > 0) {
    _exit(kExitSecondSignal);
  }
  void (*fn)(void*) = g_cancel_fn.load();
  if (fn != nullptr) {
    fn(g_cancel_arg.load());
  }
}

// Parses; on --help prints the help of the innermost subcommand. Returns the exit code to stop with, or
// nullopt to continue.
std::optional<int> parse(Cli& cli, const std::vector<std::string>& args, Context& ctx) {
  CLI::App& app = cli.app();
  const auto innermost = [&app]() {
    const CLI::App* sub = &app;
    std::string name(kName);
    while (!sub->get_subcommands().empty()) {
      sub = sub->get_subcommands().front();
      name += " " + sub->get_name();
    }
    return std::pair{sub, name};
  };
  try {
    std::vector<std::string> reversed(args.rbegin(), args.rend());
    app.parse(reversed);
  } catch (const CLI::CallForHelp&) {
    const std::string text = innermost().first->help();
    static_cast<void>(std::fwrite(text.data(), 1, text.size(), ctx.out));
    return static_cast<int>(ExitCode::Ok);
  } catch (const CLI::ParseError& e) {
    print_usage_error(ctx, UsageError{"CLI-0001", e.what(), innermost().second});
    return static_cast<int>(ExitCode::Usage);
  }
  const auto [sub, name] = innermost();
  const std::vector<std::string> extras = app.remaining(true);
  if (extras.empty()) {
    return std::nullopt;
  }
  const std::string word = extras.front().substr(0, extras.front().find('='));
  std::string message;
  if (word.starts_with('-')) {
    message = fmt::format("unknown option '{}' for '{}'", word, name);
  } else if (sub == &app) {
    message = fmt::format("unknown subcommand '{}'", word);
  } else {
    message = fmt::format("unexpected argument '{}' for '{}'", word, name);
  }
  if (const std::string hint = closest_spelling(*sub, word); !hint.empty()) {
    message += fmt::format(" (did you mean '{}'?)", hint);
  }
  print_usage_error(ctx, UsageError{"CLI-0001", message, name});
  return static_cast<int>(ExitCode::Usage);
}

// An option restricted to `accepted` (CLI::IsMember): a bad value is a CLI11 parse failure, so CLI-0001.
CLI::Option* add_choice(CLI::App& app, const std::string& name, std::string& var, const std::string& help,
                        const std::vector<std::string>& accepted) {
  return app.add_option(name, var, help)->check(CLI::IsMember(accepted));
}

int dispatch(const Cli& cli, const Globals& g, Context& ctx) {
  const bool version = cli.raw().version;
  if (!version) {
    print_usage_error(ctx, UsageError{"CLI-0001", "a subcommand is required", std::string(kName)});
    return static_cast<int>(ExitCode::Usage);
  }
  // SPEC-ASSUMPTION(#101): --version supports text and json only.
  if (const auto bad = check_format(g, "--version", {Format::Json, Format::Text})) {
    print_usage_error(ctx, *bad);
    return static_cast<int>(ExitCode::Usage);
  }
  OutputTarget target(ctx, g);
  if (auto d = target.open()) {
    report(ctx, g, *d);
    return static_cast<int>(ExitCode::Io);
  }
  const bool ok = write_version(g, target.stream());
  const ExitCode code = cancel_requested() ? ExitCode::Cancelled : ExitCode::Ok;
  const auto closed = target.close(code);
  if (!ok || closed) {
    report(
        ctx, g,
        closed.value_or(Diagnostic{"Error", "CLI-0004", fmt::format("cannot write '{}'", g.output), std::nullopt, ""}));
    return static_cast<int>(ExitCode::Io);
  }
  return static_cast<int>(code);
}

}  // namespace

std::string_view format_name(Format f) noexcept {
  switch (f) {
    case Format::Json:
      return "json";
    case Format::Fb:
      return "fb";
    case Format::Csv:
      return "csv";
    case Format::Yuview:
      return "yuview";
    case Format::AomInspect:
      return "aom-inspect";
    case Format::MvCsv:
      return "mv-csv";
    case Format::VmafJson:
      return "vmaf-json";
    case Format::Text:
      break;
  }
  return "text";
}

std::optional<Format> parse_format(std::string_view name) noexcept {
  if (name == "yuview-csv") {  // older spelling, alias of yuview (§12.2.0)
    return Format::Yuview;
  }
  for (const Format f : {Format::Text, Format::Json, Format::Fb, Format::Csv, Format::Yuview, Format::AomInspect,
                         Format::MvCsv, Format::VmafJson}) {
    if (format_name(f) == name) {
      return f;
    }
  }
  return std::nullopt;
}

unsigned effective_threads(const Globals& g) noexcept {
  if (g.threads != 0) {
    return g.threads;
  }
  return std::clamp(std::thread::hardware_concurrency(), 1U, kMaxDefaultThreads);
}

bool Cli::given(std::string_view long_name) const {
  const CLI::Option* opt = app_.get_option_no_throw("--" + std::string(long_name));
  return opt != nullptr && opt->count() > 0;
}

Cli::Cli()
    : app_("Open Video Tools: bitstream, container and quality analysis for H.264, H.265 and AV1", OVT_CLI_NAME_VALUE) {
  app_.require_subcommand(0, 1);
  app_.fallthrough();   // global options are accepted before or after the subcommand (§12.1.2)
  app_.allow_extras();  // unknown words are reported by parse() as CLI-0001 with a spelling hint
  app_.set_help_flag("-h,--help", "Help for the tool or the subcommand")->group("Global options");
  app_.option_defaults()->group("Global options");
  add_choice(app_, "-f,--format", raw_.format, "Output encoding (default: json if stdout is not a TTY, else text)",
             {"json", "fb", "csv", "yuview", "text", "aom-inspect", "mv-csv", "vmaf-json", "yuview-csv"});
  app_.add_flag("--pretty", raw_.pretty, "2-space-indented JSON regardless of the TTY test");
  app_.add_flag("--compact", raw_.compact, "Single-line JSON regardless of the TTY test");
  app_.add_option("-o,--output", raw_.output, "Write the result to a file instead of stdout");
  app_.add_option("--cache-dir", raw_.cache_dir, "Location of the on-disk cache (env OVT_CACHE_DIR)");
  app_.add_flag("--no-cache", raw_.no_cache, "Neither read nor write the cache");
  app_.add_option("--plugin-dir", raw_.plugin_dirs, "Additional plugin directory, repeatable (env OVT_PLUGIN_PATH)")
      ->allow_extra_args(false);
  app_.add_option("--threads", raw_.threads,
                  "Worker threads; 0 = hardware concurrency, capped at 16 (env OVT_THREADS)");
  add_choice(app_, "--log-level", raw_.log_level, "Log threshold for stderr, default warn (env OVT_LOG_LEVEL)",
             {"trace", "debug", "info", "warn", "error", "fatal", "off"});
  app_.add_option("--log-file", raw_.log_file, "Append log lines to this file instead of stderr");
  app_.add_flag("--partial-ok", raw_.partial_ok, "Keep a partial --output file on exit 5 or 7");
  app_.add_flag("--progress", raw_.progress, "Emit progress to stderr");
  app_.add_flag("--time", raw_.time, "Append a timing object to the result");
  app_.add_flag("-q,--quiet", raw_.quiet, "Equivalent to --log-level error");
  add_choice(app_, "--color", raw_.color, "ANSI colour in text output", {"auto", "always", "never"});
  app_.add_flag("-V,--version", raw_.version, "Print the version and exit");
  app_.add_flag("--json", raw_.json, "Alias of --format json")->group("Aliases");
  app_.add_flag("--text", raw_.text, "Alias of --format text")->group("Aliases");
  app_.add_flag("--flatbuffers", raw_.flatbuffers, "Alias of --format fb")->group("Aliases");
}

int run(const std::vector<std::string>& args, Context& ctx) {
  // FR-CLI-010; the writers are locale independent anyway. Runs before any thread exists.
  static_cast<void>(std::setlocale(LC_NUMERIC, "C"));  // NOLINT(concurrency-mt-unsafe)
  Cli cli;
  if (const auto stop = parse(cli, args, ctx)) {
    return *stop;
  }
  Resolved res = resolve(cli, ctx);
  if (res.error || !res.globals) {
    print_usage_error(ctx, res.error.value_or(UsageError{}));
    return static_cast<int>(ExitCode::Usage);
  }
  const Globals& g = *res.globals;

  LogTarget log{ctx.err, g.format == Format::Json, false, g.log_level};
  std::FILE* log_file = nullptr;
  if (!g.log_file.empty()) {
    log_file = std::fopen(g.log_file.c_str(), "ab");
    if (log_file == nullptr) {
      report(ctx, g, Diagnostic{"Error", "CLI-0004", fmt::format("cannot write '{}'", g.log_file), std::nullopt, ""});
      return static_cast<int>(ExitCode::Io);
    }
    log.file = log_file;
  }
  log.color = log_file == nullptr && !log.json &&
              (g.color == ColorMode::Always || (g.color == ColorMode::Auto && ctx.stderr_tty));
  ovt_set_log_level(g.log_level);
  ovt_set_logger(&library_log, &log);
  if (g.log_level <= OVT_LOG_DEBUG) {
    write_log_line(log, OVT_LOG_DEBUG, "cli",
                   fmt::format("{} {}: format {}, threads {}, cache {}", kName, OVT_VERSION_FULL, format_name(g.format),
                               effective_threads(g), g.no_cache ? "off" : g.cache_dir));
  }
  const int code = dispatch(cli, g, ctx);
  ovt_set_logger(nullptr, nullptr);
  if (log_file != nullptr) {
    static_cast<void>(std::fclose(log_file));
  }
  return code;
}

void install_signal_handlers() {
  struct sigaction sa{};
  sa.sa_handler = &ovt_cli_on_signal;
  sigemptyset(&sa.sa_mask);
  static_cast<void>(sigaction(SIGINT, &sa, nullptr));
  static_cast<void>(sigaction(SIGTERM, &sa, nullptr));
}

bool cancel_requested() noexcept {
  return g_signal_count.load() > 0;
}

void set_cancel_hook(void (*fn)(void*), void* arg) noexcept {
  g_cancel_arg.store(arg);
  g_cancel_fn.store(fn);
}

void reset_cancel_state_for_testing() noexcept {
  g_signal_count.store(0);
  set_cancel_hook(nullptr, nullptr);
}

}  // namespace ovt::cli
