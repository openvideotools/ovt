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

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "test_support/corpus.h"
#include "test_support/fb_normalize.h"
#include "test_support/golden.h"
#include "test_support/golden_compare.h"

namespace {

std::string shell_quote(const std::string& arg) {
  std::string out = "'";
  for (const char c : arg) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
}

int usage(const std::string& problem) {
  std::cerr << "ovt_golden_run: " << problem << "\n"
            << "usage: ovt_golden_run --golden <file> --family <name> --out <file> [--tolerances <file>] "
               "[--corpus <id>] [--updatable] -- <cmd> [args...]\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path golden;
  std::filesystem::path out;
  std::filesystem::path tolerances;
  std::string family_text;
  std::string corpus;
  std::vector<std::string> command;
  bool updatable = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--") {
      command.assign(argv + i + 1, argv + argc);
      break;
    }
    if (arg == "--updatable") {
      updatable = true;
      continue;
    }
    if (i + 1 >= argc) {
      return usage("missing value for " + arg);
    }
    const std::string value = argv[++i];
    if (arg == "--golden") {
      golden = value;
    } else if (arg == "--out") {
      out = value;
    } else if (arg == "--tolerances") {
      tolerances = value;
    } else if (arg == "--family") {
      family_text = value;
    } else if (arg == "--corpus") {
      corpus = value;
    } else {
      return usage("unknown option " + arg);
    }
  }
  if (golden.empty() || out.empty() || command.empty()) {
    return usage("--golden, --out and a command are required");
  }
  const auto family = ovt::test::family_from_name(family_text);
  if (!family) {
    return usage("unknown family '" + family_text + "'");
  }

  if (ovt::test::update_golden()) {
    if (!updatable) {
      std::cerr << "ovt_golden_run: OVT_UPDATE_GOLDEN=1 refused: " << golden
                << " is an oracle golden (register the test with UPDATABLE for an OVT-side golden)\n";
      return 1;
    }
    if (std::getenv("CI") != nullptr) {
      std::cerr << "ovt_golden_run: OVT_UPDATE_GOLDEN=1 refused: CI is set\n";
      return 1;
    }
  }

  if (!corpus.empty()) {
    const auto path = ovt::test::corpus_path(corpus);
    if (!path) {
      std::cerr << "corpus file missing: " << corpus << " (run tools/fetch_corpora.py)\n";
      return ovt::test::require_corpora() ? 1 : 77;
    }
    for (std::string& arg : command) {
      if (arg == "@CORPUS@") {
        arg = path->string();
      }
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(out.parent_path(), ec);
  std::string shell;
  for (const std::string& arg : command) {
    shell += shell_quote(arg) + " ";
  }
  shell += "> " + shell_quote(out.string());
  if (std::system(shell.c_str()) != 0) {
    std::cerr << "ovt_golden_run: command failed: " << shell << "\n";
    return 2;
  }

  const std::ifstream in(out, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  const nlohmann::json actual = nlohmann::json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
  if (actual.is_discarded()) {
    std::cerr << "ovt_golden_run: output of the command is not valid JSON: " << out << "\n";
    return 1;
  }
  if (const auto problem = ovt::test::check_family_shape(*family, actual)) {
    std::cerr << "ovt_golden_run: OVT output: " << *problem << "\n";
    return 1;
  }

  ovt::test::CompareOptions options;
  if (!tolerances.empty()) {
    try {
      options = ovt::test::load_compare_options(tolerances);
    } catch (const std::exception& e) {
      return usage(e.what());
    }
  }
  const auto result = ovt::test::compare_json_golden(golden, actual, options);
  if (!result.ok) {
    std::cerr << result.message;
    return 1;
  }
  if (result.updated) {
    std::cerr << result.message << "\n";
  }
  if (!result.updated) {
    // A malformed golden must not pass because it is a subset of everything.
    const std::ifstream golden_in(golden, std::ios::binary);
    std::ostringstream golden_text;
    golden_text << golden_in.rdbuf();
    const nlohmann::json golden_json = nlohmann::json::parse(golden_text.str(), nullptr, false);
    if (const auto problem = ovt::test::check_family_shape(*family, golden_json)) {
      std::cerr << "ovt_golden_run: golden " << golden << ": " << *problem << "\n";
      return 1;
    }
  }
  return 0;
}
