// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_golden_run: the process behind every test registered with ovt_add_golden_test() (chapter 14 §14.4.0, WI-0.556).
//
//   ovt_golden_run --golden <file> --family <name> --out <file> [--tolerances <file>] [--corpus <id>] [--updatable]
//                  -- <cmd> [args]
//
// Runs <cmd> with stdout redirected to <out> (the canonical JSON of OVT's output), replacing the argument
// "@CORPUS@" by the verified corpus file <id>, then compares <out> with <golden> by the tolerant semantics of
// golden_compare.h. OVT_UPDATE_GOLDEN=1 rewrites <golden> instead, but only for goldens registered --updatable
// (OVT-side goldens, §14.4.0 step 4) and never when the environment variable CI is set: an oracle-produced golden is
// the lower bound OVT is measured against, and overwriting it with OVT's own output would make every later run pass.
// <out> is unique per test, so `ctest -j8` never shares a scratch file.
//
// Exit: 0 pass, 1 golden mismatch / missing golden / bad output, 2 usage error or <cmd> failed, 77 skipped
// (corpus file missing and OVT_REQUIRE_CORPORA is not 1; with it, a missing corpus is exit 1).

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "test_support/corpus.h"
#include "test_support/fb_normalize.h"
#include "test_support/golden.h"
#include "test_support/golden_compare.h"

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX, needed by posix_spawnp

namespace {

struct Args {
  std::filesystem::path golden;
  std::filesystem::path out;
  std::filesystem::path tolerances;
  std::string family;
  std::string corpus;
  std::vector<std::string> command;
  bool updatable = false;
};

int usage(const std::string& problem) {
  std::cerr << "ovt_golden_run: " << problem << "\n"
            << "usage: ovt_golden_run --golden <file> --family <name> --out <file> [--tolerances <file>] "
               "[--corpus <id>] [--updatable] -- <cmd> [args...]\n";
  return 2;
}

// Returns an error text, or std::nullopt when @p args was parsed.
std::optional<std::string> parse_args(std::span<char*> argv, Args& args) {
  for (std::size_t i = 1; i < argv.size(); ++i) {
    const std::string arg = argv[i];
    if (arg == "--") {
      for (std::size_t k = i + 1; k < argv.size(); ++k) {
        args.command.emplace_back(argv[k]);
      }
      break;
    }
    if (arg == "--updatable") {
      args.updatable = true;
      continue;
    }
    if (i + 1 >= argv.size()) {
      return "missing value for " + arg;
    }
    const std::string value = argv[++i];
    if (arg == "--golden") {
      args.golden = value;
    } else if (arg == "--out") {
      args.out = value;
    } else if (arg == "--tolerances") {
      args.tolerances = value;
    } else if (arg == "--family") {
      args.family = value;
    } else if (arg == "--corpus") {
      args.corpus = value;
    } else {
      return "unknown option " + arg;
    }
  }
  if (args.golden.empty() || args.out.empty() || args.command.empty()) {
    return "--golden, --out and a command are required";
  }
  return std::nullopt;
}

// OVT_UPDATE_GOLDEN=1 rewrites OVT-side goldens only, and never on CI. Returns the refusal, or std::nullopt.
std::optional<std::string> update_refusal(const Args& args) {
  if (!ovt::test::update_golden()) {
    return std::nullopt;
  }
  if (!args.updatable) {
    return "OVT_UPDATE_GOLDEN=1 refused: " + args.golden.string() +
           " is an oracle golden (register the test with UPDATABLE for an OVT-side golden)";
  }
  // NOLINTNEXTLINE(concurrency-mt-unsafe): this tool is single-threaded.
  if (std::getenv("CI") != nullptr) {
    return "OVT_UPDATE_GOLDEN=1 refused: CI is set";
  }
  return std::nullopt;
}

// Runs @p command (no shell) with stdout redirected to @p out. Returns true when it exited with status 0.
bool run_to_file(const std::vector<std::string>& command, const std::filesystem::path& out) {
  std::vector<char*> argv;
  argv.reserve(command.size() + 1);
  for (const std::string& arg : command) {
    argv.push_back(
        const_cast<char*>(arg.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast): execvp takes char*
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    return false;
  }
  int status = 0;
  return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string read_text(const std::filesystem::path& path) {
  const std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Replaces "@CORPUS@" in the command. Returns an exit code when the run must stop here, else std::nullopt.
std::optional<int> resolve_corpus(Args& args) {
  if (args.corpus.empty()) {
    return std::nullopt;
  }
  const auto path = ovt::test::corpus_path(args.corpus);
  if (!path) {
    std::cerr << "corpus file missing: " << args.corpus << " (run tools/fetch_corpora.py)\n";
    return ovt::test::require_corpora() ? 1 : 77;
  }
  for (std::string& arg : args.command) {
    if (arg == "@CORPUS@") {
      arg = path->string();
    }
  }
  return std::nullopt;
}

int compare_with_golden(const Args& args, ovt::test::Family family) {
  const nlohmann::json actual = nlohmann::json::parse(read_text(args.out), nullptr, /*allow_exceptions=*/false);
  if (actual.is_discarded()) {
    std::cerr << "ovt_golden_run: output of the command is not valid JSON: " << args.out << "\n";
    return 1;
  }
  if (const auto problem = ovt::test::check_family_shape(family, actual)) {
    std::cerr << "ovt_golden_run: OVT output: " << *problem << "\n";
    return 1;
  }
  ovt::test::CompareOptions options;
  if (!args.tolerances.empty()) {
    options = ovt::test::load_compare_options(args.tolerances);
  }
  const auto result = ovt::test::compare_json_golden(args.golden, actual, options);
  if (!result.ok) {
    std::cerr << result.message;
    return 1;
  }
  if (result.updated) {
    std::cerr << result.message << "\n";
    return 0;
  }
  // A malformed golden must not pass because it is a subset of everything.
  const nlohmann::json golden = nlohmann::json::parse(read_text(args.golden), nullptr, /*allow_exceptions=*/false);
  if (const auto problem = ovt::test::check_family_shape(family, golden)) {
    std::cerr << "ovt_golden_run: golden " << args.golden << ": " << *problem << "\n";
    return 1;
  }
  return 0;
}

int run(int argc, char** argv) {
  Args args;
  if (const auto error = parse_args(std::span<char*>(argv, static_cast<std::size_t>(argc)), args)) {
    return usage(*error);
  }
  const auto family = ovt::test::family_from_name(args.family);
  if (!family) {
    return usage("unknown family '" + args.family + "'");
  }
  if (const auto refusal = update_refusal(args)) {
    std::cerr << "ovt_golden_run: " << *refusal << "\n";
    return 1;
  }
  if (const auto stop = resolve_corpus(args)) {
    return *stop;
  }
  std::error_code ec;
  std::filesystem::create_directories(args.out.parent_path(), ec);
  if (!run_to_file(args.command, args.out)) {
    std::cerr << "ovt_golden_run: command failed: " << args.command.front() << "\n";
    return 2;
  }
  return compare_with_golden(args, *family);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "ovt_golden_run: " << e.what() << "\n";
    return 2;
  }
}
