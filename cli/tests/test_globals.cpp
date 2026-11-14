// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.500: global options, precedence, exit codes, --version, completion, logging, signals and --progress
// (chapter 12 §12.1.2, §12.1.3, §12.2.15). The front-end runs in-process through ovt::cli::run() with
// captured streams, an injected environment and injected TTY flags.

#include <sys/wait.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "app.h"
#include "completions.h"
#include "exit_codes.h"

namespace {

using ovt::cli::ExitCode;

std::string slurp(std::FILE* f) {
  std::string s;
  std::rewind(f);
  for (int c = std::fgetc(f); c != EOF; c = std::fgetc(f)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}

struct Result {
  int code = -1;
  std::string out;
  std::string err;
};

Result invoke(const std::vector<std::string>& args, const std::map<std::string, std::string>& env = {},
              bool tty = false) {
  std::FILE* out = std::tmpfile();
  std::FILE* err = std::tmpfile();
  REQUIRE(out != nullptr);
  REQUIRE(err != nullptr);
  ovt::cli::Context ctx;
  ctx.out = out;
  ctx.err = err;
  ctx.stdout_tty = tty;
  ctx.stderr_tty = false;
  ctx.getenv = [&env](const std::string& name) -> std::optional<std::string> {
    const auto it = env.find(name);
    return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
  };
  ctx.completions = ovt::cli::embedded_completions();
  Result r;
  r.code = ovt::cli::run(args, ctx);
  r.out = slurp(out);
  r.err = slurp(err);
  static_cast<void>(std::fclose(out));
  static_cast<void>(std::fclose(err));
  return r;
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("AT1: ovt without arguments exits 2 and points to --help", "[fast][cli]") {
  const Result r = invoke({});
  CHECK(r.code == 2);
  CHECK(r.out.empty());
  CHECK(r.err.ends_with("See 'ovt --help'.\n"));
}

TEST_CASE("AT2: ovt --version --format json has the keys ovt core api build features", "[fast][cli]") {
  const Result r = invoke({"--version", "--format", "json"});
  REQUIRE(r.code == 0);
  const auto j = nlohmann::json::parse(r.out);
  CHECK(j.size() == 5);
  for (const char* key : {"ovt", "core", "api", "build", "features"}) {
    CHECK(j.contains(key));
  }
  CHECK(j["build"].contains("commit"));
  CHECK(j["features"].is_array());
}

TEST_CASE("AT3: --format fb on a terminal is refused with exit 2", "[fast][cli]") {
  const Result r = invoke({"--format", "fb", "--version"}, {}, true);
  CHECK(r.code == 2);
  CHECK(r.out.empty());
  CHECK(contains(r.err, "error: CLI-0010 refusing to write binary to a terminal; use -o"));
}

TEST_CASE("AT4: --log-level error wins over OVT_LOG_LEVEL=trace", "[fast][cli]") {
  const Result flag = invoke({"--log-level", "error", "--version"}, {{"OVT_LOG_LEVEL", "trace"}});
  CHECK(flag.code == 0);
  CHECK(flag.err.empty());
  // Control: the environment alone does enable the startup debug line.
  const Result env = invoke({"--version"}, {{"OVT_LOG_LEVEL", "trace"}});
  CHECK(env.code == 0);
  CHECK_FALSE(env.err.empty());
}

TEST_CASE("AT5: ovt completion zsh begins with compdef", "[fast][cli]") {
  const Result zsh = invoke({"completion", "zsh"});
  CHECK(zsh.code == 0);
  CHECK(zsh.out.starts_with("#compdef ovt\n"));
  CHECK(contains(invoke({"completion", "bash"}).out, "complete -F _ovt ovt"));
  CHECK(contains(invoke({"completion", "fish"}).out, "complete -c ovt"));
}

TEST_CASE("completion on a terminal ignores OVT_FORMAT=fb", "[fast][cli]") {
  const Result env = invoke({"completion", "zsh"}, {{"OVT_FORMAT", "fb"}}, true);
  CHECK(env.code == 0);
  CHECK(env.out.starts_with("#compdef ovt\n"));
  CHECK(env.err.empty());
  const Result flag = invoke({"--format", "fb", "completion", "zsh"}, {}, true);
  CHECK(flag.code == 2);
  CHECK(contains(flag.err, "CLI-0010"));
}

TEST_CASE("Completion scripts list subcommands and IsMember choices", "[fast][cli]") {
  const std::string bash = invoke({"completion", "bash"}).out;
  CHECK(contains(bash, "completion"));
  CHECK(contains(bash, "json fb csv yuview text aom-inspect mv-csv vmaf-json"));
  CHECK_FALSE(contains(bash, "yuview-csv"));
  CHECK(contains(bash, "bash zsh fish"));
  CHECK(contains(invoke({"completion", "zsh"}).out, "--flatbuffers"));
}

TEST_CASE("Completion offers file names only for path options", "[fast][cli]") {
  const std::string bash = invoke({"completion", "bash"}).out;
  CHECK(contains(bash, "--threads) COMPREPLY=(); return ;;"));
  CHECK(contains(bash, "-o|--output) COMPREPLY=($(compgen -f"));
  const std::string zsh = invoke({"completion", "zsh"}).out;
  CHECK(contains(zsh, "(env OVT_THREADS)]:value: '"));
  CHECK(contains(zsh, "instead of stdout]:value:_files'"));
  const std::string fish = invoke({"completion", "fish"}).out;
  CHECK(contains(fish, "-l threads -x"));
  CHECK(contains(fish, "-l output -r -F"));
}

TEST_CASE("Version text on a terminal names core and api", "[fast][cli]") {
  const Result r = invoke({"-V"}, {}, true);
  CHECK(r.code == 0);
  CHECK(r.out.starts_with("ovt "));
  CHECK(contains(r.out, "(core "));
  CHECK(contains(r.out, ", api 1."));
}

TEST_CASE("Format default follows the TTY test and OVT_FORMAT", "[fast][cli]") {
  CHECK(invoke({"--version"}).out.starts_with("{"));
  CHECK(invoke({"--version"}, {}, true).out.starts_with("ovt "));
  CHECK(invoke({"--version"}, {{"OVT_FORMAT", "text"}}).out.starts_with("ovt "));
  CHECK(invoke({"--version", "--json"}, {{"OVT_FORMAT", "text"}}).out.starts_with("{"));
  CHECK(invoke({"--version"}, {{"OVT_FORMAT", "bogus"}}).code == 2);
}

TEST_CASE("JSON layout follows the TTY test unless pretty or compact is given", "[fast][cli]") {
  CHECK(invoke({"--version", "--json"}, {}, true).out.starts_with("{\n  \"ovt\": "));
  CHECK(invoke({"--version"}).out.starts_with("{\"ovt\":"));
  CHECK(invoke({"--version", "--pretty"}).out.starts_with("{\n"));
  const Result both = invoke({"--version", "--pretty", "--compact"});
  CHECK(both.code == 2);
  CHECK(contains(both.err, "CLI-0006 options --pretty and --compact are mutually exclusive"));
}

TEST_CASE("An alias that disagrees with --format is CLI-0006", "[fast][cli]") {
  const Result r = invoke({"--json", "--format", "csv", "--version"});
  CHECK(r.code == 2);
  CHECK(contains(r.err, "error: CLI-0006 options --format csv and --json are mutually exclusive"));
  CHECK(invoke({"--json", "--format", "json", "--version"}).code == 0);
}

TEST_CASE("An unsupported format is CLI-0002 naming the supported ones", "[fast][cli]") {
  const Result r = invoke({"--format", "csv", "--version"});
  CHECK(r.code == 2);
  CHECK(contains(r.err, "CLI-0002 format 'csv' is not supported by '--version'; supported: json, text"));
  const Result c = invoke({"completion", "fish", "-f", "json"});
  CHECK(c.code == 2);
  CHECK(contains(c.err, "supported: text. See 'ovt completion --help'."));
  CHECK(invoke({"completion", "fish"}, {{"OVT_FORMAT", "json"}}).code == 0);  // only a flag is checked
}

TEST_CASE("Unknown options and subcommands are CLI-0001 with a spelling hint", "[fast][cli]") {
  const Result opt = invoke({"--jsno", "--version"});
  CHECK(opt.code == 2);
  CHECK(contains(opt.err, "error: CLI-0001 unknown option '--jsno' for 'ovt' (did you mean '--json'?)"));
  const Result sub = invoke({"completoin"});
  CHECK(sub.code == 2);
  CHECK(contains(sub.err, "unknown subcommand 'completoin' (did you mean 'completion'?). See 'ovt --help'."));
  CHECK(invoke({"completion", "tcsh"}).code == 2);
}

TEST_CASE("Global options are accepted after the subcommand", "[fast][cli]") {
  const Result r = invoke({"completion", "bash", "--text", "--log-level", "off"});
  CHECK(r.code == 0);
  CHECK(r.out.starts_with("# bash completion"));
  const Result bad = invoke({"completion", "bash", "--bogus"});
  CHECK(bad.code == 2);
  CHECK(contains(bad.err, "unknown option '--bogus' for 'ovt completion'"));
}

TEST_CASE("Help prints to stdout and exits 0", "[fast][cli]") {
  const Result top = invoke({"--help"});
  CHECK(top.code == 0);
  CHECK(contains(top.out, "completion"));
  CHECK(contains(top.out, "Aliases"));
  const Result sub = invoke({"completion", "--help"});
  CHECK(sub.code == 0);
  CHECK(contains(sub.out, "bash"));
}

TEST_CASE("Flags beat environment variables beat defaults", "[fast][cli]") {
  const std::map<std::string, std::string> env = {
      {"OVT_THREADS", "3"}, {"OVT_CACHE_DIR", "/env/cache"}, {"OVT_LOG_LEVEL", "debug"}};
  const Result from_env = invoke({"--version"}, env);
  CHECK(contains(from_env.err, "threads 3, cache /env/cache"));
  const Result from_flag = invoke({"--version", "--threads", "5", "--cache-dir", "/flag"}, env);
  CHECK(contains(from_flag.err, "threads 5, cache /flag"));
  const Result xdg = invoke({"--version", "--log-level", "debug"}, {{"XDG_CACHE_HOME", "/x"}, {"HOME", "/h"}});
#if !defined(__APPLE__)
  CHECK(contains(xdg.err, "cache /x/ovt/"));
#endif
  CHECK(invoke({"--version", "-q"}, env).err.empty());
  const Result bad_env = invoke({"--version"}, {{"OVT_THREADS", "many"}});
  CHECK(bad_env.code == 2);
  CHECK(contains(bad_env.err, "error: CLI-0011 invalid value 'many' in OVT_THREADS"));
}

TEST_CASE("Log lines are JSON Lines under --format json", "[fast][cli]") {
  const Result r = invoke({"--version", "--log-level", "debug"});
  REQUIRE(r.code == 0);
  const auto line = nlohmann::json::parse(r.err.substr(0, r.err.find('\n')));
  CHECK(line["level"] == "debug");
  CHECK(line["module"] == "cli");
  const Result text = invoke({"--version", "--log-level", "debug", "--text"});
  CHECK(contains(text.err, "Z DEBUG cli ovt "));
}

TEST_CASE("Output goes to -o and an unwritable path is CLI-0004 exit 3", "[fast][cli]") {
  const auto path = std::filesystem::temp_directory_path() / "ovt_cli_test_version.json";
  const Result r = invoke({"--version", "-o", path.string()});
  CHECK(r.code == 0);
  CHECK(r.out.empty());
  CHECK(std::filesystem::file_size(path) > 2);
  std::filesystem::remove(path);
  const Result bad = invoke({"--version", "-o", "/nonexistent-dir/x.json"});
  CHECK(bad.code == 3);
  CHECK(contains(bad.err, "\"code\":\"CLI-0004\""));
}

TEST_CASE("Exit codes map from ovt_status as in the table", "[fast][cli]") {
  using ovt::cli::from_status;
  CHECK(from_status(OVT_OK) == ExitCode::Ok);
  CHECK(from_status(OVT_E_INVALID_ARG) == ExitCode::Usage);
  CHECK(from_status(OVT_E_IO) == ExitCode::Io);
  CHECK(from_status(OVT_E_CORRUPT) == ExitCode::Corrupt);
  CHECK(from_status(OVT_E_CANCELLED) == ExitCode::Cancelled);
  CHECK(from_status(OVT_E_NOMEM) == ExitCode::Error);
}

TEST_CASE("Exit code numbers and names", "[fast][cli]") {
  using ovt::cli::exit_code_name;
  CHECK(static_cast<int>(ExitCode::Unsupported) == 4);
  CHECK(static_cast<int>(ExitCode::Plugin) == 6);
  CHECK(static_cast<int>(ExitCode::Partial) == 9);
  CHECK(exit_code_name(ExitCode::Mismatch) == "MISMATCH");
  CHECK(ovt::cli::from_status(OVT_E_UNSUPPORTED) == ExitCode::Unsupported);
  CHECK(ovt::cli::from_status(OVT_E_PLUGIN) == ExitCode::Plugin);
}

namespace {
std::atomic<int> g_hook_calls{0};
void count_hook(void* /*arg*/) {
  g_hook_calls.fetch_add(1);
}
}  // namespace

TEST_CASE("The first SIGINT sets the flag and calls the cancel hook", "[fast][cli]") {
  ovt::cli::reset_cancel_state_for_testing();
  ovt::cli::install_signal_handlers();
  ovt::cli::set_cancel_hook(&count_hook, nullptr);
  CHECK_FALSE(ovt::cli::cancel_requested());
  REQUIRE(std::raise(SIGINT) == 0);
  CHECK(ovt::cli::cancel_requested());
  CHECK(g_hook_calls.load() == 1);
  ovt::cli::reset_cancel_state_for_testing();
  static_cast<void>(std::signal(SIGINT, SIG_DFL));
  static_cast<void>(std::signal(SIGTERM, SIG_DFL));
}

TEST_CASE("A second signal exits with 130", "[fast][cli]") {
  const pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ovt::cli::install_signal_handlers();
    static_cast<void>(std::raise(SIGTERM));
    static_cast<void>(std::raise(SIGINT));
    _exit(0);  // not reached
  }
  int status = 0;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 130);
}

TEST_CASE("Progress without a TTY is one JSON line per percent step", "[fast][cli]") {
  std::FILE* err = std::tmpfile();
  REQUIRE(err != nullptr);
  ovt::cli::ProgressReporter p(err, false, "index", "frames", [] { return 0.0; });
  p.update(0.421, 1200, 2850);
  p.update(0.425, 1210, 2850);  // same percent: no line
  p.update(0.43, 1225, 2850);
  p.finish();
  const std::string s = slurp(err);
  static_cast<void>(std::fclose(err));
  CHECK(s ==
        "{\"progress\":0.42,\"done\":1200,\"total\":2850,\"stage\":\"index\"}\n"
        "{\"progress\":0.43,\"done\":1225,\"total\":2850,\"stage\":\"index\"}\n");
}

TEST_CASE("Progress on a TTY rewrites one line with an ETA", "[fast][cli]") {
  std::FILE* err = std::tmpfile();
  REQUIRE(err != nullptr);
  double now = 0.0;
  ovt::cli::ProgressReporter p(err, true, "index", "frames", [&now] { return now; });
  now = 8.7;
  p.update(0.42, 1200, 2850);
  p.finish();
  const std::string s = slurp(err);
  static_cast<void>(std::fclose(err));
  CHECK(s == "\r[####      ] 42% frames 1200/2850 ETA 00:12\x1b[K\n");
}
