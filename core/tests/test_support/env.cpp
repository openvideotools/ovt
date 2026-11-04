// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_support/env.h"

#include <cstdlib>

namespace ovt::test {

std::optional<std::string> env_value(const char* name) {
  const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read-only, before threads start
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

bool env_flag(const char* name) {
  const auto value = env_value(name);
  return value && *value == "1";
}

}  // namespace ovt::test
