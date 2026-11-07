// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// macOS default plugin directories (chapter 7 §7.5.1, chapter 12 §12.3.11, FR-MAC-006; WI-2.150).
// SPEC-ASSUMPTION(#212): Contents/PlugIns/ovt/ (FR-MAC-006), then Contents/PlugIns/ (chapter 13 §13.2.6), then
// the exe-relative and chapter 12 directories.

#include <mach-o/dyld.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"

namespace ovt::analysis {

namespace {

std::filesystem::path executable_path() {
  uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) {
    return {};
  }
  buf.resize(std::char_traits<char>::length(buf.c_str()));
  std::error_code ec;
  std::filesystem::path p = std::filesystem::canonical(buf, ec);
  return ec ? std::filesystem::path(buf) : p;
}

}  // namespace

std::vector<std::string> platform_default_plugin_dirs() {
  std::vector<std::string> dirs;
  const std::filesystem::path exe = executable_path();
  if (exe.has_parent_path()) {
    const std::filesystem::path exe_dir = exe.parent_path();
    // <App>.app/Contents/MacOS/<exe>
    if (exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents") {
      const std::filesystem::path contents = exe_dir.parent_path();
      dirs.push_back((contents / "PlugIns" / "ovt").string());
      dirs.push_back((contents / "PlugIns").string());
    }
    dirs.push_back((exe_dir / ".." / "lib" / "ovt" / "plugins").lexically_normal().string());
  }
  const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): read-only lookup
  if (home != nullptr && *home != '\0') {
    dirs.push_back(std::string(home) + "/Library/Application Support/ovt/plugins");
  }
  dirs.emplace_back("/usr/local/lib/ovt/plugins");
  dirs.emplace_back("/opt/homebrew/lib/ovt/plugins");  // Homebrew prefix on Apple silicon
  return dirs;
}

// Same signature as the Android implementation, which keeps the strings.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
void set_android_plugin_dirs(std::string /*native_library_dir*/, std::string /*files_plugin_dir*/) {}

}  // namespace ovt::analysis
