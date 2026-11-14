// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Android default plugin directories (chapter 7 §7.5.1, chapter 13 §13.4.6; WI-2.150): the app's
// nativeLibraryDir, then <filesDir>/plugins, both passed from Kotlin before the first scan. The codec-pack
// signature check (ANA-PLG-0003) and the W^X copy into codeCacheDir belong to WI-0.053.

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"

namespace ovt::analysis {

namespace {

struct AndroidDirs {
  std::mutex mutex;
  std::vector<std::string> dirs;
};
AndroidDirs& android_dirs() {
  static AndroidDirs d;
  return d;
}

}  // namespace

std::vector<std::string> platform_default_plugin_dirs() {
  AndroidDirs& d = android_dirs();
  const std::lock_guard lock(d.mutex);
  return d.dirs;
}

void set_android_plugin_dirs(std::string native_library_dir, std::string files_plugin_dir) {
  AndroidDirs& d = android_dirs();
  const std::lock_guard lock(d.mutex);
  d.dirs.clear();
  for (std::string* dir : {&native_library_dir, &files_plugin_dir}) {
    if (!dir->empty()) {
      d.dirs.push_back(std::move(*dir));
    }
  }
}

}  // namespace ovt::analysis
