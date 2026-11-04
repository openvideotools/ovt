// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_support/corpus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <system_error>

#include "test_support/env.h"

#ifndef OVT_TEST_DEFAULT_CORPORA_DIR
#define OVT_TEST_DEFAULT_CORPORA_DIR "build/corpora"
#endif

namespace ovt::test {

std::filesystem::path corpora_dir() {
  if (auto env = env_value("OVT_CORPORA_DIR"); env && !env->empty()) {
    return std::filesystem::path(*env);
  }
  return std::filesystem::path(OVT_TEST_DEFAULT_CORPORA_DIR);
}

std::optional<std::filesystem::path> corpus_path(std::string_view id) {
  if (id.empty() || id.find('/') != std::string_view::npos || id.find('\\') != std::string_view::npos ||
      id == "." || id == "..") {
    return std::nullopt;
  }
  const std::filesystem::path dir = corpora_dir() / std::filesystem::path(std::string(id));
  std::error_code ec;
  if (!std::filesystem::is_regular_file(dir / ".ok", ec)) {
    return std::nullopt;
  }
  std::optional<std::filesystem::path> found;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().filename() == ".ok") {
      continue;
    }
    if (found) {
      return std::nullopt;  // ambiguous: the layout holds exactly one file per entry
    }
    found = entry.path();
  }
  return found;
}

bool require_corpora() { return env_flag("OVT_REQUIRE_CORPORA"); }

std::filesystem::path corpus_or_skip(std::string_view id) {
  if (auto path = corpus_path(id)) {
    return *path;
  }
  const std::string message = "corpus file missing: " + std::string(id);
  if (require_corpora()) {
    FAIL(message);
  }
  SKIP(message);
  return {};  // not reached: SKIP and FAIL throw
}

}  // namespace ovt::test
