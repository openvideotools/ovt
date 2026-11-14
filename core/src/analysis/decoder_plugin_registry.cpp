// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Plugin discovery, loading and ABI validation (chapter 7 §7.5.1, chapter 12 §12.3.11; WI-2.150).
//
// FR-GEN-007: only directories from the search path are ever dlopen()ed; nothing derived from an input file
// reaches this code. A plugin file is never dlopen()ed when its id is already registered, so no foreign code runs
// for a file that would be ignored anyway.
// SPEC-ASSUMPTION(#212): search order, ownership check, duplicate handling and defaults.

#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "ovt/cxx/log.hpp"
#include "ovt_model_generated.h"

namespace ovt::analysis {

namespace {

constexpr std::string_view kModule = "analysis";
constexpr std::string_view kPrefix = "libovt_plugin_";
#if defined(__APPLE__)
constexpr std::string_view kSuffix = ".dylib";
#else
constexpr std::string_view kSuffix = ".so";
#endif
constexpr std::size_t kMaxIdLength = 32;
constexpr const char* kEntryPoint = "ovt_analysis_decoder_v1";

// Every member of the v1 table must lie inside the struct the plugin reports (struct_size, D4).
constexpr std::size_t kApiV1Size = offsetof(ovt_AnalysisDecoderApi, flush) + sizeof(ovt_AnalysisDecoderApi::flush);
constexpr std::size_t kApiVersionEnd =
    offsetof(ovt_AnalysisDecoderApi, abi_version) + sizeof(ovt_AnalysisDecoderApi::abi_version);
constexpr std::size_t kCapsV1Size = offsetof(ovt_AnalysisCaps, notes) + sizeof(ovt_AnalysisCaps::notes);

using EntryPoint = const ovt_AnalysisDecoderApi* (*)();

// Calls a plugin function that returns a pointer. Plugins MUST NOT let an exception escape (§7.5.1 "Sandboxing");
// the catch is the host's best-effort guard and cannot make unwinding through C frames defined.
template <class F>
auto guarded_call(F fn, bool& threw) noexcept -> decltype(fn()) {
  try {
    return fn();
  } catch (...) {
    threw = true;
    return nullptr;
  }
}

std::string dl_error() {
  const char* e = dlerror();  // NOLINT(concurrency-mt-unsafe): the registry mutex serialises every dl* call
  return e != nullptr ? std::string(e) : std::string("unknown dlerror");
}

bool is_valid_plugin_id(std::string_view id) noexcept {
  return !id.empty() && id.size() <= kMaxIdLength && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

bool is_valid_codec(uint8_t codec) noexcept {
  return codec > static_cast<uint8_t>(ovt::fb::Codec::Unknown) && codec <= static_cast<uint8_t>(ovt::fb::Codec::MAX);
}

// Built-in default order per codec (chapter 12 §12.3.11, which extends chapter 7 §7.5.1).
std::vector<std::string_view> builtin_order(uint8_t codec) {
  switch (static_cast<ovt::fb::Codec>(codec)) {
    case ovt::fb::Codec::AV1:
      return {"aom"};
    case ovt::fb::Codec::H265:
      return {"de265", "hm", "ffmpeg"};
    case ovt::fb::Codec::H264:
      return {"ffmpeg"};
    default:
      return {};
  }
}

// Plugins that are used only when asked for by id or by set_default(), never as the fallback of default_for(): the
// optional ffmpeg_hevc motion vector / QP plugin must never be the default H.265 plugin (chapter 07 §7.8 WI-4.107).
// SPEC-ASSUMPTION(#370): the registry enforces this by id, since §7.8 does not say where the rule lives.
bool opt_in_only(std::string_view id) {
  return id == "ffmpeg_hevc";
}

// The libovt_plugin_* files of one directory as (path, id), sorted so that registration order is deterministic.
std::vector<std::pair<std::string, std::string>> list_plugin_files(const std::string& dir) {
  std::vector<std::pair<std::string, std::string>> files;
  std::error_code ec;  // a missing or unreadable directory just yields no files
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    auto id = plugin_id_from_file_name(it->path().filename().string());
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

// dlopen()s `path` and resolves its function table. On failure the library is closed again and `error` says why.
struct OpenedPlugin {
  void* handle = nullptr;
  const ovt_AnalysisDecoderApi* api = nullptr;
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
  } else if (dlsym(handle, "ovt_metric_v1") != nullptr) {
    error = "exports both ovt_analysis_decoder_v1 and ovt_metric_v1";  // §12.3.11: exactly one entry point
  } else {
    bool threw = false;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): dlsym returns the function as void*
    const ovt_AnalysisDecoderApi* api = guarded_call(reinterpret_cast<EntryPoint>(sym), threw);
    if (api != nullptr) {
      return {handle, api};
    }
    error = std::string(kEntryPoint) + (threw ? "() threw an exception" : "() returned NULL");
  }
  dlclose(handle);
  return {};
}

// Checks the function table and caps (D4 struct_size first, then §7.5.1 rules). Returns the caps, or nullptr
// with `error` (and `code` for ANA-PLG-0001) set.
const ovt_AnalysisCaps* validate_plugin(const ovt_AnalysisDecoderApi* api, const std::string& expected_id,
                                        std::string& code, std::string& error) {
  if (api->struct_size < kApiVersionEnd) {  // abi_version is read only once struct_size shows it is there
    error = "struct_size " + std::to_string(api->struct_size) + " too small for abi_version";
    return nullptr;
  }
  if (api->abi_version != OVT_ANALYSIS_ABI_VERSION) {
    code = "ANA-PLG-0001";
    error = "rejected: abi_version " + std::to_string(api->abi_version) + ", host expects " +
            std::to_string(OVT_ANALYSIS_ABI_VERSION);
    return nullptr;
  }
  if (api->struct_size < kApiV1Size) {
    error =
        "ovt_AnalysisDecoderApi.struct_size " + std::to_string(api->struct_size) + " < " + std::to_string(kApiV1Size);
    return nullptr;
  }
  if (api->caps == nullptr || api->create == nullptr || api->destroy == nullptr || api->push == nullptr ||
      api->pull == nullptr || api->plane == nullptr || api->release == nullptr || api->flush == nullptr) {
    error = "function table has a NULL entry";
    return nullptr;
  }
  bool threw = false;
  const ovt_AnalysisCaps* caps = guarded_call(api->caps, threw);
  if (caps == nullptr) {
    error = threw ? "caps() threw an exception" : "caps() returned NULL";
  } else if (caps->struct_size < kCapsV1Size) {
    error = "ovt_AnalysisCaps.struct_size " + std::to_string(caps->struct_size) + " < " + std::to_string(kCapsV1Size);
  } else if (!is_valid_codec(caps->codec)) {
    error = "caps->codec " + std::to_string(caps->codec) + " is not an ovt.fb.Codec";
  } else if (caps->id == nullptr || !is_valid_plugin_id(caps->id)) {
    error = "caps->id is missing or not [a-z0-9_]{1,32}";
  } else if (expected_id != caps->id) {
    error = std::string("caps->id '") + caps->id + "' differs from the file name id '" + expected_id + "'";
  } else if (caps->version == nullptr || caps->license == nullptr) {
    error = "caps->version and caps->license must be non-null";
  } else {
    return caps;
  }
  return nullptr;
}

}  // namespace

std::optional<std::string> plugin_id_from_file_name(std::string_view file_name) {
  if (file_name.size() <= kPrefix.size() + kSuffix.size() || file_name.substr(0, kPrefix.size()) != kPrefix ||
      file_name.substr(file_name.size() - kSuffix.size()) != kSuffix) {
    return std::nullopt;
  }
  const std::string_view id = file_name.substr(kPrefix.size(), file_name.size() - kPrefix.size() - kSuffix.size());
  return is_valid_plugin_id(id) ? std::optional<std::string>(id) : std::nullopt;
}

std::vector<std::string> resolve_plugin_search_path(const std::vector<std::string>& added, const char* ovt_plugin_path,
                                                    const std::vector<std::string>& defaults) {
  std::vector<std::string> out;
  auto push = [&out](std::string dir) {
    if (!dir.empty() && std::find(out.begin(), out.end(), dir) == out.end()) {
      out.push_back(std::move(dir));
    }
  };
  for (const auto& dir : added) {
    push(dir);
  }
  const std::string env = ovt_plugin_path != nullptr ? std::string(ovt_plugin_path) : std::string();
  for (std::size_t start = 0, colon = 0; !env.empty() && colon != std::string::npos; start = colon + 1) {
    colon = env.find(':', start);
    push(env.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
  }
  // §7.5.1: OVT_PLUGIN_PATH replaces the defaults unless it ends in ':' (which appends them).
  if (env.empty() || env.back() == ':') {
    for (const auto& dir : defaults) {
      push(dir);
    }
  }
  return out;
}

PluginRegistry& PluginRegistry::instance() {
  static PluginRegistry registry;  // never destroyed before exit; plugins stay loaded (§12.3.11)
  return registry;
}

PluginRegistry::PluginRegistry() = default;

PluginRegistry::PluginRegistry(Options options) : options_(std::move(options)) {}

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
  const char* env = options_.ovt_plugin_path.has_value()
                        ? options_.ovt_plugin_path->c_str()
                        : std::getenv("OVT_PLUGIN_PATH");  // NOLINT(concurrency-mt-unsafe): read-only lookup
  const std::vector<std::string> defaults =
      options_.defaults.has_value() ? *options_.defaults : platform_default_plugin_dirs();
  return resolve_plugin_search_path(added, env, defaults);
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
        (void)load_file_locked(path, id, nullptr);
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
      seen_files_.insert(p->path);  // already loaded: not dlopen()ed a second time
    }
  }
  return scan();
}

ovt_status PluginRegistry::load_file_locked(const std::string& path, const std::string& file_id,
                                            const LoadedPlugin** out) {
  // Duplicate id: the first directory wins and the later file is never dlopen()ed (§7.5.1).
  if (const LoadedPlugin* first = find_locked(file_id); first != nullptr) {
    if (first->path == path) {
      if (out != nullptr) {
        *out = first;
      }
      return OVT_OK;
    }
    const std::string msg = "plugin " + path + " ignored: id " + file_id + " already registered from " + first->path;
    reject_locked(path, "ANA-PLG-0002", msg, ovt::log::Level::Debug);  // §7.5.1: logged at level 1
    return OVT_E_PLUGIN;
  }
  std::string error;
  const OpenedPlugin opened = open_plugin(path, error);
  if (opened.api == nullptr) {
    const std::string msg = "plugin " + path + ": " + error;
    reject_locked(path, "", msg, ovt::log::Level::Warn);
    return OVT_E_PLUGIN;
  }
  const ovt_status st = register_locked(opened.api, path, opened.handle, file_id, out);
  if (st != OVT_OK) {
    dlclose(opened.handle);
  }
  return st;
}

ovt_status PluginRegistry::register_locked(const ovt_AnalysisDecoderApi* api, std::string path, void* handle,
                                           const std::string& expected_id, const LoadedPlugin** out) {
  std::string code;
  std::string error;
  // caps->id equals the file name id, which load_file_locked() already found unregistered.
  const ovt_AnalysisCaps* caps = validate_plugin(api, expected_id, code, error);
  if (caps == nullptr) {
    // The ANA-PLG-0001 text follows the chapter 7 §7.9 registry ("plugin %s rejected: ..."); logged at level 3.
    const std::string msg = "plugin " + path + (code.empty() ? ": " : " ") + error;
    reject_locked(std::move(path), std::move(code), msg, ovt::log::Level::Warn);
    return OVT_E_PLUGIN;
  }
  auto plugin = std::make_unique<LoadedPlugin>();
  plugin->id = caps->id;
  plugin->path = std::move(path);
  plugin->handle = handle;
  plugin->api = api;
  plugin->caps = caps;
  OVT_LOG(ovt::log::Level::Info, kModule, "plugin {} {} ({}) loaded from {}", plugin->id, caps->version, caps->license,
          plugin->path);
  plugins_.push_back(std::move(plugin));
  if (out != nullptr) {
    *out = plugins_.back().get();
  }
  return OVT_OK;
}

void PluginRegistry::reject_locked(std::string path, std::string code, std::string message, ovt::log::Level level) {
  if (ovt::log::enabled(level)) {
    ovt::log::write(level, kModule, code, message);
  }
  rejections_.push_back(PluginRejection{std::move(path), std::move(code), std::move(message)});
}

const LoadedPlugin* PluginRegistry::find_locked(std::string_view id) const {
  for (const auto& p : plugins_) {
    if (p->id == id) {
      return p.get();
    }
  }
  return nullptr;
}

std::vector<const LoadedPlugin*> PluginRegistry::plugins() {
  (void)scan();
  const std::lock_guard lock(mutex_);
  std::vector<const LoadedPlugin*> out;
  out.reserve(plugins_.size());
  for (const auto& p : plugins_) {
    out.push_back(p.get());
  }
  return out;
}

const LoadedPlugin* PluginRegistry::find(std::string_view id) {
  (void)scan();
  const std::lock_guard lock(mutex_);
  return find_locked(id);
}

const LoadedPlugin* PluginRegistry::default_for(uint8_t codec) {
  (void)scan();
  const std::lock_guard lock(mutex_);
  auto for_codec = [&](std::string_view id) -> const LoadedPlugin* {
    const LoadedPlugin* p = find_locked(id);
    return p != nullptr && p->caps->codec == codec ? p : nullptr;
  };
  if (auto it = default_overrides_.find(codec); it != default_overrides_.end()) {
    if (const LoadedPlugin* p = for_codec(it->second); p != nullptr) {
      return p;
    }
  }
  for (const std::string_view id : builtin_order(codec)) {
    if (const LoadedPlugin* p = for_codec(id); p != nullptr) {
      return p;
    }
  }
  for (const auto& p : plugins_) {
    if (p->caps->codec == codec && !opt_in_only(p->caps->id)) {
      return p.get();
    }
  }
  return nullptr;
}

ovt_status PluginRegistry::set_default(uint8_t codec, std::string_view plugin_id) {
  const std::lock_guard lock(mutex_);
  if (plugin_id.empty()) {
    default_overrides_.erase(codec);
    return OVT_OK;
  }
  if (!is_valid_plugin_id(plugin_id)) {
    return OVT_E_INVALID_ARG;
  }
  default_overrides_[codec] = std::string(plugin_id);
  return OVT_OK;
}

std::vector<PluginRejection> PluginRegistry::rejections() const {
  const std::lock_guard lock(mutex_);
  return rejections_;
}

}  // namespace ovt::analysis
