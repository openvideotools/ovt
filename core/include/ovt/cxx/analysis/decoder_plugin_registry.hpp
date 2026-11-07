// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::analysis::PluginRegistry: discovery, loading and ABI validation of analysis decoder plugins (chapter 7
// §7.5.1; chapter 12 §12.3.11 loading rules; WI-2.150). C++ helper header (D22): no ABI promise, never included
// from a C header, not installed. WI-2.520 wraps it in the C API of ovt/plugins.h and adds load_file and static
// registration there.
//
// SPEC-ASSUMPTION(#212): search order, the ownership check, duplicate handling and default_for() as proposed
// in the issue.

#ifndef OVT_CXX_ANALYSIS_DECODER_PLUGIN_REGISTRY_HPP
#define OVT_CXX_ANALYSIS_DECODER_PLUGIN_REGISTRY_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/log.hpp"
#include "ovt/status.h"

namespace ovt::analysis {

// One registered plugin. Owned by the registry and never destroyed or moved before the registry is: a plugin
// is never unloaded once registered (chapter 12 §12.3.11: decoders may keep static state), so pointers to a
// LoadedPlugin stay valid for the life of the registry (the process, for instance()).
struct LoadedPlugin {
  std::string id;                               // caps->id, equal to <id> of the file name
  std::string path;                             // file path as found on the search path
  void* handle = nullptr;                       // dlopen() handle
  const ovt_AnalysisDecoderApi* api = nullptr;  // validated function table
  const ovt_AnalysisCaps* caps = nullptr;       // validated caps
};

// A plugin file that was found but not registered, with the diagnostic code of chapter 7 §7.9.
struct PluginRejection {
  std::string path;
  std::string code;     // "ANA-PLG-0001" (ABI version), "ANA-PLG-0002" (duplicate id) or "" (anything else)
  std::string message;  // the logged text, including dlerror() where there is one
};

// Platform default plugin directories, in search order (chapter 7 §7.5.1, chapter 12 §12.3.11), from
// plugin_paths_{linux,macos,android}.cpp. Directories that do not exist are kept; scanning skips them.
[[nodiscard]] std::vector<std::string> platform_default_plugin_dirs();

// Android: the directories passed from Kotlin (ApplicationInfo.nativeLibraryDir, <filesDir>/plugins), which
// platform_default_plugin_dirs() returns on Android. A no-op elsewhere. Call before the first scan.
void set_android_plugin_dirs(std::string native_library_dir, std::string files_plugin_dir);

// The search path: `added` first, then the colon-separated `ovt_plugin_path` ($OVT_PLUGIN_PATH; empty
// entries are skipped), then `defaults` only when `ovt_plugin_path` is null, empty or ends in ':'. Duplicates
// keep their first position. Pure function, exposed for the tests.
[[nodiscard]] std::vector<std::string> resolve_plugin_search_path(const std::vector<std::string>& added,
                                                                  const char* ovt_plugin_path,
                                                                  const std::vector<std::string>& defaults);

// The <id> of `libovt_plugin_<id>.<ext>` when `file_name` matches that pattern with `[a-z0-9_]{1,32}` and the
// platform's extension (.dylib on macOS, .so elsewhere); std::nullopt otherwise.
[[nodiscard]] std::optional<std::string> plugin_id_from_file_name(std::string_view file_name);

// Whether `id` is a valid plugin id, `[a-z0-9_]{1,32}`.
[[nodiscard]] bool is_valid_plugin_id(std::string_view id) noexcept;

class PluginRegistry {
 public:
  // The process singleton (§7.5.1), built lazily; it scans on the first call to plugins()/find()/default_for().
  [[nodiscard]] static PluginRegistry& instance();

  // A private registry, for tests: `ovt_plugin_path` replaces $OVT_PLUGIN_PATH ("" = unset), `defaults` the
  // platform defaults.
  struct Options {
    std::optional<std::string> ovt_plugin_path;
    std::optional<std::vector<std::string>> defaults;
  };
  PluginRegistry();
  explicit PluginRegistry(Options options);
  // Registered plugins are not dlclose()d (chapter 12 §12.3.11).
  ~PluginRegistry() = default;
  PluginRegistry(const PluginRegistry&) = delete;
  PluginRegistry& operator=(const PluginRegistry&) = delete;
  PluginRegistry(PluginRegistry&&) = delete;
  PluginRegistry& operator=(PluginRegistry&&) = delete;

  // Adds a directory searched before $OVT_PLUGIN_PATH (ovt_plugins_add_search_path), used from the next
  // scan()/rescan(). OVT_E_INVALID_ARG for an empty string.
  ovt_status add_search_path(std::string dir);

  // The resolved search path (added paths, $OVT_PLUGIN_PATH, defaults), in order.
  [[nodiscard]] std::vector<std::string> search_paths() const;

  // Loads every libovt_plugin_* file of the search path; idempotent until rescan(). The first directory holding
  // an id wins; a later file with that id is logged (ANA-PLG-0002) and never dlopen()ed. Rejected files are
  // logged and recorded in rejections(). Thread-safe; OVT_OK even when files were rejected.
  ovt_status scan();

  // Forgets the cached directory listing and scans again (ovt_analysis_rescan_plugins). Already-registered
  // plugins stay loaded; previously rejected files are retried.
  ovt_status rescan();

  // Registered plugins in registration order. Scans first if no scan has run.
  [[nodiscard]] std::vector<const LoadedPlugin*> plugins();
  // The plugin with this id, or nullptr. Scans first if no scan has run.
  [[nodiscard]] const LoadedPlugin* find(std::string_view id);
  // The plugin used for `codec` (an ovt.fb.Codec value) when ovt_analysis_open gets no id: the override set
  // with set_default() if it is registered for that codec; else the first registered id of the built-in order
  // (AV1: aom; H265: de265, hm, ffmpeg; H264: ffmpeg) whose caps report that codec; else the first registered
  // plugin for that codec; else nullptr (DEC-PLUGIN-0001 is the caller's diagnostic). Scans first if needed.
  [[nodiscard]] const LoadedPlugin* default_for(uint8_t codec);

  // The user setting analysis.default_plugin.<codec> (ovt_plugins_set_default). An empty id clears it.
  // OVT_E_INVALID_ARG when the id is not a valid plugin id.
  ovt_status set_default(uint8_t codec, std::string_view plugin_id);

  // Files rejected since construction, oldest first.
  [[nodiscard]] std::vector<PluginRejection> rejections() const;

 private:
  ovt_status load_file_locked(const std::string& path, const std::string& file_id, const LoadedPlugin** out);
  ovt_status register_locked(const ovt_AnalysisDecoderApi* api, std::string path, void* handle,
                             const std::string& expected_id, const LoadedPlugin** out);
  // Logs the rejection at `level` with its diagnostic code and appends it to rejections().
  void reject_locked(std::string path, std::string code, std::string message, ovt::log::Level level);
  [[nodiscard]] const LoadedPlugin* find_locked(std::string_view id) const;

  Options options_;
  mutable std::mutex mutex_;
  std::vector<std::string> added_paths_;
  std::vector<std::unique_ptr<LoadedPlugin>> plugins_;
  std::vector<PluginRejection> rejections_;
  std::set<std::string> seen_files_;  // files already attempted since the last rescan
  std::map<uint8_t, std::string> default_overrides_;
  bool scanned_ = false;
};

}  // namespace ovt::analysis

#endif  // OVT_CXX_ANALYSIS_DECODER_PLUGIN_REGISTRY_HPP
