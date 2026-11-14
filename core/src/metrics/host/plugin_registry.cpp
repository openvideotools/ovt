// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Metric plugin discovery, loading and ABI validation (chapter 9 §9.2.2, chapter 12 §12.3.11; WI-3.300 step 3).
// Mirrors ovt::analysis::PluginRegistry (WI-2.150), with the metric ABI and the VQ-PLUG-0001 diagnostic.
//
// FR-GEN-007: only directories from the search path are ever dlopen()ed; nothing derived from an input file
// reaches this code.

#include "metrics/host/plugin_registry.h"

#include "metrics/kernels/psnr_metric.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <flatbuffers/flatbuffers.h>
#include <nlohmann/json.hpp>

#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt_metrics_generated.h"

namespace ovt::metrics {

namespace {

constexpr std::string_view kModule = "metrics";
constexpr std::string_view kCodeRejected = "VQ-PLUG-0001";
constexpr std::string_view kPrefix = "libovt_metric_";
#if defined(__APPLE__)
constexpr std::string_view kSuffix = ".dylib";
#else
constexpr std::string_view kSuffix = ".so";
#endif
constexpr std::size_t kMaxIdLength = 32;
constexpr const char* kEntryPoint = "ovt_metric_v1";
constexpr const char* kOtherEntryPoint = "ovt_analysis_decoder_v1";

// Every member of the v1 table must lie inside the struct the plugin reports (struct_size, D4).
constexpr std::size_t kApiV1Size = offsetof(ovt_MetricApi, finish) + sizeof(ovt_MetricApi::finish);
constexpr std::size_t kApiVersionEnd = offsetof(ovt_MetricApi, abi_version) + sizeof(ovt_MetricApi::abi_version);

using EntryPoint = const ovt_MetricApi* (*)();

template <class F>
auto guarded_call(F fn, bool& threw) noexcept -> decltype(fn()) {
  try {
    return fn();
  } catch (...) {
    threw = true;
    return decltype(fn()){};
  }
}

std::string dl_error() {
  const char* e = dlerror();  // NOLINT(concurrency-mt-unsafe): the registry mutex serialises every dl* call
  return e != nullptr ? std::string(e) : std::string("unknown dlerror");
}

bool is_valid_metric_id(std::string_view id) noexcept {
  return !id.empty() && id.size() <= kMaxIdLength && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

std::vector<std::pair<std::string, std::string>> list_plugin_files(const std::string& dir) {
  std::vector<std::pair<std::string, std::string>> files;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    auto id = metric_plugin_id_from_file_name(it->path().filename().string());
    std::error_code fec;
    if (id && it->is_regular_file(fec)) {
      files.emplace_back(it->path().string(), std::move(*id));
    }
  }
  if (ec) {
    OVT_LOG(ovt::log::Level::Debug, kModule, "cannot list plugin directory {}: {}", dir, ec.message());
  }
  std::sort(files.begin(), files.end());
  return files;
}

struct OpenedPlugin {
  void* handle = nullptr;
  const ovt_MetricApi* api = nullptr;
};
OpenedPlugin open_plugin(const std::string& path, std::string& error) {
  // NOLINTNEXTLINE(hicpp-signed-bitwise): POSIX flag constants
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    error = "dlopen failed: " + dl_error();
    return {};
  }
  (void)dlerror();  // NOLINT(concurrency-mt-unsafe): see dl_error()
  void* sym = dlsym(handle, kEntryPoint);
  if (sym == nullptr) {
    error = std::string("no symbol ") + kEntryPoint + ": " + dl_error();
  } else if (dlsym(handle, kOtherEntryPoint) != nullptr) {
    error = "exports both ovt_metric_v1 and ovt_analysis_decoder_v1";  // §12.3.11: exactly one entry point
  } else {
    bool threw = false;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): dlsym returns the function as void*
    const ovt_MetricApi* api = guarded_call(reinterpret_cast<EntryPoint>(sym), threw);
    if (api != nullptr) {
      return {handle, api};
    }
    error = std::string(kEntryPoint) + (threw ? "() threw an exception" : "() returned NULL");
  }
  dlclose(handle);
  return {};
}

std::string enum_name(ovt::fb::MetricKind k) {
  const char* n = ovt::fb::EnumNameMetricKind(k);
  return (n != nullptr && *n != '\0') ? n : std::to_string(static_cast<int>(k));
}
std::string enum_name(ovt::fb::WorkingDomain d) {
  const char* n = ovt::fb::EnumNameWorkingDomain(d);
  return (n != nullptr && *n != '\0') ? n : std::to_string(static_cast<int>(d));
}

// One MetricDescriptor as a JSON object: schema field order, enum names, absent strings as "".
nlohmann::ordered_json descriptor_json(const ovt::fb::MetricDescriptor& d) {
  nlohmann::ordered_json j;
  j["id"] = d.id() != nullptr ? d.id()->str() : std::string();
  j["display_name"] = d.display_name() != nullptr ? d.display_name()->str() : std::string();
  j["kind"] = enum_name(d.kind());
  j["domain"] = enum_name(d.domain());
  j["per_plane"] = d.per_plane();
  j["higher_is_better"] = d.higher_is_better();
  j["range_min"] = d.range_min();
  j["range_max"] = d.range_max();
  j["temporal_unit_frames"] = d.temporal_unit_frames();
  nlohmann::ordered_json outputs = nlohmann::ordered_json::array();
  if (d.outputs() != nullptr) {
    for (const auto* s : *d.outputs()) {
      outputs.push_back(s != nullptr ? s->str() : std::string());
    }
  }
  j["outputs"] = std::move(outputs);
  j["license"] = d.license() != nullptr ? d.license()->str() : std::string();
  j["version"] = d.version() != nullptr ? d.version()->str() : std::string();
  return j;
}

// nlohmann::json::dump() throws on a string that is not valid UTF-8, which would fail
// ovt_metrics_list_descriptors() for every metric because of one bad plugin. The FlatBuffers verifier does not
// check encodings, so a descriptor whose strings do not dump is rejected at registration instead.
bool descriptor_dumps_as_json(const ovt::fb::MetricDescriptor& d) noexcept {
  try {
    (void)descriptor_json(d).dump();
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

const ovt::fb::MetricDescriptor* MetricPlugin::descriptor() const noexcept {
  return descriptor_fb.empty() ? nullptr : flatbuffers::GetRoot<ovt::fb::MetricDescriptor>(descriptor_fb.data());
}

std::optional<std::string> metric_plugin_id_from_file_name(std::string_view file_name) {
  if (file_name.size() <= kPrefix.size() + kSuffix.size() || file_name.substr(0, kPrefix.size()) != kPrefix ||
      file_name.substr(file_name.size() - kSuffix.size()) != kSuffix) {
    return std::nullopt;
  }
  const std::string_view id = file_name.substr(kPrefix.size(), file_name.size() - kPrefix.size() - kSuffix.size());
  return is_valid_metric_id(id) ? std::optional<std::string>(id) : std::nullopt;
}

std::optional<std::vector<uint8_t>> validate_metric_api(const ovt_MetricApi* api, const std::string* expected_id,
                                                        std::string& error) {
  if (api == nullptr) {
    error = "ovt_metric_v1() returned NULL";
    return std::nullopt;
  }
  if (api->struct_size < kApiVersionEnd) {  // abi_version is read only once struct_size shows it is there
    error = "struct_size " + std::to_string(api->struct_size) + " too small for abi_version";
    return std::nullopt;
  }
  if (api->abi_version != OVT_METRIC_ABI_VERSION) {
    error =
        "abi_version " + std::to_string(api->abi_version) + ", host expects " + std::to_string(OVT_METRIC_ABI_VERSION);
    return std::nullopt;
  }
  if (api->struct_size < kApiV1Size) {
    error = "ovt_MetricApi.struct_size " + std::to_string(api->struct_size) + " < " + std::to_string(kApiV1Size);
    return std::nullopt;
  }
  if (api->descriptor == nullptr || api->create == nullptr || api->destroy == nullptr || api->score == nullptr ||
      api->finish == nullptr) {
    error = "function table has a NULL entry";
    return std::nullopt;
  }
  bool threw = false;
  const ovt_Buffer buf = guarded_call(api->descriptor, threw);
  if (threw) {
    error = "descriptor() threw an exception";
    return std::nullopt;
  }
  if (buf.data == nullptr || buf.size == 0) {
    error = "descriptor() returned an empty buffer";
    return std::nullopt;
  }
  flatbuffers::Verifier verifier(buf.data, buf.size);
  if (!verifier.VerifyBuffer<ovt::fb::MetricDescriptor>(nullptr)) {
    error = "descriptor() is not a valid MetricDescriptor FlatBuffer";
    return std::nullopt;
  }
  const auto* d = flatbuffers::GetRoot<ovt::fb::MetricDescriptor>(buf.data);
  if (d->id() == nullptr || !is_valid_metric_id(d->id()->string_view())) {
    error = "descriptor id is missing or not [a-z0-9_]{1,32}";
    return std::nullopt;
  }
  if (expected_id != nullptr && *expected_id != d->id()->string_view()) {
    error = "descriptor id '" + d->id()->str() + "' differs from the file name id '" + *expected_id + "'";
    return std::nullopt;
  }
  if (d->kind() > ovt::fb::MetricKind::MAX || d->domain() > ovt::fb::WorkingDomain::MAX) {
    error = "descriptor kind or domain out of range";
    return std::nullopt;
  }
  if (!descriptor_dumps_as_json(*d)) {
    error = "descriptor strings are not valid UTF-8";
    return std::nullopt;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): copying a verified buffer
  return std::vector<uint8_t>(buf.data, buf.data + buf.size);
}

PluginRegistry& PluginRegistry::instance() {
  // Never destroyed: plugins stay loaded (§12.3.11).
  static PluginRegistry* const registry = [] {
    auto* r = new PluginRegistry();  // NOLINT(cppcoreguidelines-owning-memory)
    register_builtin_kernels(*r);    // the in-tree kernel metrics (WI-3.306 onward)
    return r;
  }();
  return *registry;
}

PluginRegistry::PluginRegistry() = default;

PluginRegistry::PluginRegistry(Options options) : options_(std::move(options)) {}

ovt_status PluginRegistry::register_builtin(const ovt_MetricApi* api, std::string origin) {
  const std::lock_guard lock(mutex_);
  return register_locked(api, std::string(), std::move(origin), nullptr, nullptr);
}

ovt_status PluginRegistry::add_search_path(std::string dir) {
  if (dir.empty()) {
    return OVT_E_INVALID_ARG;
  }
  const std::lock_guard lock(mutex_);
  if (std::find(added_paths_.begin(), added_paths_.end(), dir) == added_paths_.end()) {
    added_paths_.push_back(std::move(dir));
  }
  return OVT_OK;
}

std::vector<std::string> PluginRegistry::search_paths() const {
  std::vector<std::string> added;
  {
    const std::lock_guard lock(mutex_);
    added = added_paths_;
  }
  // $OVT_METRIC_PATH entries follow the added paths and only add directories.
  const char* metric_env = options_.ovt_metric_path.has_value()
                               ? options_.ovt_metric_path->c_str()
                               : std::getenv("OVT_METRIC_PATH");  // NOLINT(concurrency-mt-unsafe): read-only
  if (metric_env != nullptr) {
    const std::string env(metric_env);
    for (std::size_t start = 0, colon = 0; !env.empty() && colon != std::string::npos; start = colon + 1) {
      colon = env.find(':', start);
      const std::string dir = env.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
      if (!dir.empty() && std::find(added.begin(), added.end(), dir) == added.end()) {
        added.push_back(dir);
      }
    }
  }
  const char* plugin_env = options_.ovt_plugin_path.has_value()
                               ? options_.ovt_plugin_path->c_str()
                               : std::getenv("OVT_PLUGIN_PATH");  // NOLINT(concurrency-mt-unsafe): read-only
  const std::vector<std::string> defaults =
      options_.defaults.has_value() ? *options_.defaults : ovt::analysis::platform_default_plugin_dirs();
  return ovt::analysis::resolve_plugin_search_path(added, plugin_env, defaults);
}

ovt_status PluginRegistry::scan() {
  const std::vector<std::string> dirs = search_paths();
  const std::lock_guard lock(mutex_);
  if (scanned_) {
    return OVT_OK;
  }
  scanned_ = true;
  for (const auto& dir : dirs) {
    for (const auto& [path, id] : list_plugin_files(dir)) {
      if (seen_files_.insert(path).second) {
        (void)load_file_locked(path, id);
      }
    }
  }
  return OVT_OK;
}

ovt_status PluginRegistry::rescan() {
  {
    const std::lock_guard lock(mutex_);
    scanned_ = false;
    seen_files_.clear();
    for (const auto& p : plugins_) {
      if (!p->path.empty()) {
        seen_files_.insert(p->path);  // already loaded: not dlopen()ed a second time
      }
    }
  }
  return scan();
}

ovt_status PluginRegistry::load_file_locked(const std::string& path, const std::string& file_id) {
  if (const MetricPlugin* first = find_locked(file_id); first != nullptr) {
    if (first->path == path) {
      return OVT_OK;
    }
    const std::string from = first->path.empty() ? first->origin : first->path;
    reject_locked(path, "", "metric plugin " + path + " ignored: id " + file_id + " already registered from " + from);
    return OVT_E_PLUGIN;
  }
  std::string error;
  const OpenedPlugin opened = open_plugin(path, error);
  if (opened.api == nullptr) {
    reject_locked(path, std::string(kCodeRejected), "metric plugin " + path + " rejected: " + error);
    return OVT_E_PLUGIN;
  }
  const std::string origin = std::filesystem::path(path).parent_path().string();
  const ovt_status st = register_locked(opened.api, path, origin, opened.handle, &file_id);
  if (st != OVT_OK) {
    dlclose(opened.handle);
  }
  return st;
}

ovt_status PluginRegistry::register_locked(const ovt_MetricApi* api, std::string path, std::string origin, void* handle,
                                           const std::string* expected_id) {
  std::string error;
  std::optional<std::vector<uint8_t>> descriptor = validate_metric_api(api, expected_id, error);
  const std::string where = path.empty() ? origin : path;
  if (!descriptor) {
    reject_locked(where, std::string(kCodeRejected), "metric plugin " + where + " rejected: " + error);
    return path.empty() ? OVT_E_INVALID_ARG : OVT_E_PLUGIN;
  }
  const auto* d = flatbuffers::GetRoot<ovt::fb::MetricDescriptor>(descriptor->data());
  const std::string id = d->id()->str();
  if (const MetricPlugin* first = find_locked(id); first != nullptr) {
    const std::string from = first->path.empty() ? first->origin : first->path;
    reject_locked(where, "", "metric " + id + " from " + where + " ignored: already registered from " + from);
    return OVT_E_PLUGIN;
  }
  auto plugin = std::make_unique<MetricPlugin>();
  plugin->id = id;
  plugin->path = std::move(path);
  plugin->origin = std::move(origin);
  plugin->handle = handle;
  plugin->api = api;
  plugin->descriptor_fb = std::move(*descriptor);
  OVT_LOG(ovt::log::Level::Info, kModule, "metric {} {} ({}) registered from {}", plugin->id,
          d->version() != nullptr ? d->version()->str() : std::string("?"),
          d->license() != nullptr ? d->license()->str() : std::string("?"), where);
  plugins_.push_back(std::move(plugin));
  return OVT_OK;
}

void PluginRegistry::reject_locked(std::string path, std::string code, std::string message) {
  const auto level = code.empty() ? ovt::log::Level::Debug : ovt::log::Level::Warn;
  if (ovt::log::enabled(level)) {
    ovt::log::write(level, kModule, code, message);
  }
  rejections_.push_back(PluginRejection{std::move(path), std::move(code), std::move(message)});
}

const MetricPlugin* PluginRegistry::find_locked(std::string_view id) const {
  for (const auto& p : plugins_) {
    if (p->id == id) {
      return p.get();
    }
  }
  return nullptr;
}

std::vector<const MetricPlugin*> PluginRegistry::sorted_locked() const {
  std::vector<const MetricPlugin*> out;
  out.reserve(plugins_.size());
  for (const auto& p : plugins_) {
    out.push_back(p.get());
  }
  std::sort(out.begin(), out.end(), [](const MetricPlugin* a, const MetricPlugin* b) { return a->id < b->id; });
  return out;
}

std::vector<const MetricPlugin*> PluginRegistry::plugins() {
  (void)scan();
  const std::lock_guard lock(mutex_);
  return sorted_locked();
}

const MetricPlugin* PluginRegistry::find(std::string_view id) {
  (void)scan();
  const std::lock_guard lock(mutex_);
  return find_locked(id);
}

std::string PluginRegistry::descriptors_json() {
  (void)scan();
  const std::lock_guard lock(mutex_);
  nlohmann::ordered_json arr = nlohmann::ordered_json::array();
  for (const MetricPlugin* p : sorted_locked()) {
    arr.push_back(descriptor_json(*p->descriptor()));
  }
  return arr.dump();
}

std::vector<PluginRejection> PluginRegistry::rejections() const {
  const std::lock_guard lock(mutex_);
  return rejections_;
}

}  // namespace ovt::metrics
