// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Environment access for the test support library (OVT_CORPORA_DIR, OVT_REQUIRE_CORPORA,
// OVT_UPDATE_GOLDEN; chapter 14 §14.1.3 allows no other variables in tests).

#pragma once

#include <optional>
#include <string>

namespace ovt::test {

/// The value of environment variable @p name, or std::nullopt when unset.
std::optional<std::string> env_value(const char* name);

/// True when environment variable @p name is exactly "1".
bool env_flag(const char* name);

}  // namespace ovt::test
