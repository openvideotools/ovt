// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Corpus lookup for tests (chapter 14 §14.2.4, WI-0.550). Corpus files are never committed;
// tools/fetch_corpora.py downloads them into <cache>/<id>/<file> and writes <cache>/<id>/.ok after the
// SHA-256 check. Tests find them by corpus id.

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace ovt::test {

/// The corpus cache directory: $OVT_CORPORA_DIR if set and non-empty, otherwise the OVT_CORPORA_DIR
/// CMake option compiled into the test support library (default build/corpora).
std::filesystem::path corpora_dir();

/// The verified file of corpus entry @p id: the file named by "primary" in the JSON record <cache>/<id>/.ok
/// (written by tools/fetch_corpora.py); for a bare .ok marker, the single regular file other than .ok. std::nullopt
/// when the entry is absent or unverified.
std::optional<std::filesystem::path> corpus_path(std::string_view id);

/// True when OVT_REQUIRE_CORPORA=1: a missing corpus file fails the test instead of skipping it.
bool require_corpora();

/// For tests tagged [needs-corpus]: returns corpus_path(id) when present. Otherwise the current test is
/// skipped (Catch2 SKIP), or fails with "corpus file missing: <id>" when OVT_REQUIRE_CORPORA=1.
std::filesystem::path corpus_or_skip(std::string_view id);

}  // namespace ovt::test
