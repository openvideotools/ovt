// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Linux default plugin directories (chapter 7 §7.5.1, chapter 12 §12.3.11; WI-2.150).
// SPEC-ASSUMPTION(#212): exe-relative first, then the chapter 12 list in its order.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"

namespace ovt::analysis {

namespace {
std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read-only lookup
  return v != nullptr ? std::string(v) : std::string();
}
}  // namespace

std::vector<std::string> platform_default_plugin_dirs() {
  std::vector<std::string> dirs;
  std::error_code ec;
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec && exe.has_parent_path()) {
    dirs.push_back((exe.parent_path() / ".." / "lib" / "ovt" / "plugins").lexically_normal().string());
  }
  std::string xdg = env_or_empty("XDG_DATA_HOME");
  if (xdg.empty()) {
    if (const std::string home = env_or_empty("HOME"); !home.empty()) {
      xdg = home + "/.local/share";
    }
  }
  if (!xdg.empty()) {
    dirs.push_back(xdg + "/ovt/plugins");
  }
  dirs.emplace_back("/usr/local/lib/ovt/plugins");
  dirs.emplace_back("/usr/lib/ovt/plugins");
  dirs.emplace_back("/usr/lib64/ovt/plugins");
#if defined(__x86_64__)
  dirs.emplace_back("/usr/lib/x86_64-linux-gnu/ovt/plugins");
#elif defined(__aarch64__)
  dirs.emplace_back("/usr/lib/aarch64-linux-gnu/ovt/plugins");
#endif
  dirs.emplace_back("/app/lib/ovt/plugins");  // Flatpak
  if (const std::string appdir = env_or_empty("APPDIR"); !appdir.empty()) {
    dirs.push_back(appdir + "/usr/lib/ovt/plugins");  // AppImage
  }
  return dirs;
}

// Same signature as the Android implementation, which keeps the strings.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
void set_android_plugin_dirs(std::string /*native_library_dir*/, std::string /*files_plugin_dir*/) {}

}  // namespace ovt::analysis
