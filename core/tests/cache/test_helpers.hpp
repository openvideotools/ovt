// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared fixtures of the cache tests (WI-0.091): temporary cache roots.

#ifndef OVT_TESTS_CACHE_TEST_HELPERS_HPP
#define OVT_TESTS_CACHE_TEST_HELPERS_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace ovt::cache::test {

namespace fs = std::filesystem;

/// A fresh directory below the build tree, removed on destruction.
class TempRoot {
 public:
  explicit TempRoot(std::string_view name) : path_(fs::path(OVT_TEST_BINARY_DIR) / "tmp" / std::string(name)) {
    fs::remove_all(path_);
    fs::create_directories(path_.parent_path());
  }
  ~TempRoot() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;
  TempRoot(TempRoot&&) = delete;
  TempRoot& operator=(TempRoot&&) = delete;
  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

 private:
  fs::path path_;
};

}  // namespace ovt::cache::test

#endif  // OVT_TESTS_CACHE_TEST_HELPERS_HPP
