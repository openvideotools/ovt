// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::metrics::PluginRegistry: the metric plugins the host knows (chapter 9 §9.2.2; WI-3.300 step 3).
// Internal header of the metrics module (not installed). Two sources of plugins:
//  - static registration of the in-tree metrics (the kernels of WI-3.306 onward, the libvmaf host of WI-3.309):
//    register_builtin() with the ovt_MetricApi table the module exports;
//  - dlopen() of `libovt_metric_<id>.{so,dylib}` found on the search path: the directories added with
//    add_search_path(), then `$OVT_METRIC_PATH`, then `$OVT_PLUGIN_PATH`, then the platform plugin directories of
//    chapter 12 §12.3.11 (metric plugins share OVT_PLUGIN_DIR with the analysis plugins, chapter 13 §13.2.6; there
//    is no share/ovt/metrics/). `$OVT_PLUGIN_PATH` replaces the platform defaults unless it ends in ':' (the
//    analysis registry's rule); `$OVT_METRIC_PATH` only adds directories.
// A file that cannot be loaded is logged with VQ-PLUG-0001 (§9.11.4) and recorded in rejections(); the first
// directory holding an id wins and a later file with that id is never dlopen()ed. Plugins are never unloaded
// (§12.3.11).

#ifndef OVT_SRC_METRICS_HOST_PLUGIN_REGISTRY_H
#define OVT_SRC_METRICS_HOST_PLUGIN_REGISTRY_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/abi/metric_v1.h"
#include "ovt/status.h"

namespace ovt::fb {
struct MetricDescriptor;
}  // namespace ovt::fb

namespace ovt::metrics {

// One registered metric. Owned by the registry and never destroyed before it, so pointers stay valid for the
// life of the registry (the process, for instance()).
struct MetricPlugin {
  std::string id;                      // descriptor id, equal to <id> of the file name for a loaded plugin
  std::string path;                    // file path for a loaded plugin; empty for a built-in
  std::string origin;                  // "(built-in)" or the directory the file was found in
  void* handle = nullptr;              // dlopen() handle; nullptr for a built-in
  const ovt_MetricApi* api = nullptr;  // validated function table
  std::vector<uint8_t> descriptor_fb;  // verified copy of descriptor() (ovt.fb.MetricDescriptor root)
  [[nodiscard]] const ovt::fb::MetricDescriptor* descriptor() const noexcept;
};

// A plugin file that was found but not registered (VQ-PLUG-0001 unless the file is a duplicate id).
struct PluginRejection {
  std::string path;
  std::string code;  // "VQ-PLUG-0001", or "" for a duplicate id (logged at Debug)
  std::string message;
};

// The <id> of `libovt_metric_<id>.<ext>` when `file_name` matches that pattern with `[a-z0-9_]{1,32}` and the
// platform's extension (.dylib on macOS, .so elsewhere); std::nullopt otherwise.
[[nodiscard]] std::optional<std::string> metric_plugin_id_from_file_name(std::string_view file_name);

// Validates a function table against the ABI (struct_size, abi_version, non-null entries, descriptor()
// verifying as a MetricDescriptor with a valid id). Returns the verified descriptor bytes, or nullopt with
// `error` set. `expected_id` (nullable) must equal the descriptor id. Exposed for the tests.
[[nodiscard]] std::optional<std::vector<uint8_t>> validate_metric_api(const ovt_MetricApi* api,
                                                                      const std::string* expected_id,
                                                                      std::string& error);

class PluginRegistry {
 public:
  // The process singleton behind ovt_metrics_list_descriptors(); scans on the first query.
  [[nodiscard]] static PluginRegistry& instance();

  // A private registry, for tests: the optionals replace $OVT_METRIC_PATH, $OVT_PLUGIN_PATH ("" = unset) and the
  // platform defaults.
  struct Options {
    std::optional<std::string> ovt_metric_path;
    std::optional<std::string> ovt_plugin_path;
    std::optional<std::vector<std::string>> defaults;
  };
  PluginRegistry();
  explicit PluginRegistry(Options options);
  ~PluginRegistry() = default;  // loaded plugins are not dlclose()d (§12.3.11)
  PluginRegistry(const PluginRegistry&) = delete;
  PluginRegistry& operator=(const PluginRegistry&) = delete;
  PluginRegistry(PluginRegistry&&) = delete;
  PluginRegistry& operator=(PluginRegistry&&) = delete;

  // Registers an in-tree metric. OVT_E_INVALID_ARG when api is null or fails validation (message logged with
  // VQ-PLUG-0001 and `origin` as the path); OVT_E_PLUGIN when the id is already registered. Thread-safe.
  ovt_status register_builtin(const ovt_MetricApi* api, std::string origin);

  // Adds a directory searched first, from the next scan()/rescan(). OVT_E_INVALID_ARG for an empty string.
  ovt_status add_search_path(std::string dir);
  // The resolved search path, in order.
  [[nodiscard]] std::vector<std::string> search_paths() const;

  // Loads every libovt_metric_* file of the search path; idempotent until rescan(). Thread-safe; OVT_OK even
  // when files were rejected.
  ovt_status scan();
  // Forgets the directory listing and scans again; registered plugins stay, rejected files are retried.
  ovt_status rescan();

  // Registered metrics sorted by id. Scans first if no scan has run.
  [[nodiscard]] std::vector<const MetricPlugin*> plugins();
  // The metric with this id, or nullptr. Scans first if no scan has run.
  [[nodiscard]] const MetricPlugin* find(std::string_view id);
  // The ovt_metrics_list_descriptors() document: a JSON array of the descriptors sorted by id, each object with
  // the ovt.fb.MetricDescriptor fields in schema order and enum values as names. Scans first if needed.
  [[nodiscard]] std::string descriptors_json();

  // Files rejected since construction, oldest first.
  [[nodiscard]] std::vector<PluginRejection> rejections() const;

 private:
  ovt_status load_file_locked(const std::string& path, const std::string& file_id);
  ovt_status register_locked(const ovt_MetricApi* api, std::string path, std::string origin, void* handle,
                             const std::string* expected_id);
  void reject_locked(std::string path, std::string code, std::string message);
  [[nodiscard]] const MetricPlugin* find_locked(std::string_view id) const;
  [[nodiscard]] std::vector<const MetricPlugin*> sorted_locked() const;

  Options options_;
  mutable std::mutex mutex_;
  std::vector<std::string> added_paths_;
  std::vector<std::unique_ptr<MetricPlugin>> plugins_;
  std::vector<PluginRejection> rejections_;
  std::set<std::string> seen_files_;
  bool scanned_ = false;
};

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_HOST_PLUGIN_REGISTRY_H
