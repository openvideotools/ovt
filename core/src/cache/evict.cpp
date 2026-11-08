// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Size-cap eviction (chapter 06 §6.7.4), clear and list of the cache (WI-0.092).

#include "cache/evict.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "cache/atomic_file.hpp"
#include "ovt/cxx/cache/cache.hpp"
#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/status.h"

namespace ovt::cache {

namespace fs = std::filesystem;

namespace {

constexpr const char* kModule = "cache";
constexpr const char* kMetaJson = "meta.json";
constexpr std::size_t kMaxJsonBytes = 64U << 20U;
constexpr std::string_view kIndex = "index";
constexpr std::string_view kAnalysis = "analysis";
constexpr std::string_view kMetrics = "metrics";
constexpr std::array<std::string_view, 3> kKinds = {kAnalysis, kIndex, kMetrics};  // sorted

// Rewrites <dir>/meta.json after files were deleted from a closed entry, so that it describes what is left.
// Best effort: an unreadable meta.json stays as it is (the entry then fails I2 at its next open_entry).
void rewrite_meta(const fs::path& dir, const std::function<void(Meta&)>& edit) {
  try {
    Meta meta = parse_meta(read_file(dir / kMetaJson, kMaxJsonBytes));
    const Meta before = meta;
    edit(meta);
    if (meta != before) {
      const std::string text = serialize_meta(meta);
      write_file_atomic(dir / kMetaJson,
                        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }
  } catch (const CacheError& e) {
    OVT_LOG(log::Level::Info, kModule, "meta.json of '{}' not updated: {}", dir.string(), e.what());
  }
}

void forget_planes(Meta& m) {
  if (m.planes_cached.has_value()) {
    m.planes_cached = false;
  }
}

void forget_frames(Meta& m) {
  m.plugins.clear();
}

void forget_index(Meta& m) {
  if (m.index_complete.has_value()) {
    m.index_complete = false;
  }
}

// Deletes @p path (file or tree); the bytes it held.
std::uint64_t remove_tree(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return 0;
  }
  const std::uint64_t bytes = fs::is_directory(path, ec) ? directory_bytes(path) : fs::file_size(path, ec);
  fs::remove_all(path, ec);
  if (ec) {
    OVT_LOG(log::Level::Warn, kModule, "cannot delete '{}': {}", path.string(), ec.message());
  }
  return bytes;
}

// Deletes the files of @p kind below @p dir and the directories that become empty; the bytes freed.
std::uint64_t remove_kind(const fs::path& dir, std::string_view kind) {
  std::vector<fs::path> victims;
  std::error_code ec;
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && kind_of(e.path().lexically_relative(dir)) == kind) {
      victims.push_back(e.path());
    }
  }
  std::uint64_t freed = 0;
  for (const fs::path& p : victims) {
    freed += remove_tree(p);
  }
  for (const char* sub : {"frames", "planes", "maps", "metrics", "reports"}) {
    const fs::path p = dir / sub;
    if (fs::is_directory(p, ec)) {
      bool empty = true;
      for (const fs::directory_entry& e : fs::recursive_directory_iterator(p, ec)) {
        if (!e.is_directory(ec)) {
          empty = false;
          break;
        }
      }
      if (empty) {
        fs::remove_all(p, ec);
      }
    }
  }
  if (kind == kIndex) {
    rewrite_meta(dir, forget_index);
  } else if (kind == kAnalysis) {
    rewrite_meta(dir, [](Meta& m) {
      forget_frames(m);
      forget_planes(m);
    });
  }
  return freed;
}

// True when nothing but meta.json (and empty directories) is left in @p dir.
bool only_meta_left(const fs::path& dir) {
  std::error_code ec;
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir, ec)) {
    if (!e.is_directory(ec) && e.path().lexically_relative(dir) != kMetaJson) {
      return false;
    }
  }
  return true;
}

std::optional<SourceHash> key_of(const std::string& hex_hash) {
  return parse_hex(hex_hash);
}

void push_key(std::vector<SourceHash>& out, const std::string& hex_hash) {
  if (const auto key = key_of(hex_hash)) {
    out.push_back(*key);
  }
}

// Steps (2) of the §6.7.4 loop on one closed entry: planes/, then maps/, then frames/, each only while
// @p total is above @p target. Keeps @p r and @p total in step; true when the entry got under the target and
// stays, false when the whole directory has to go.
bool trim_entry(const fs::path& dir, const std::string& hash, CacheRecord& r, std::uint64_t& total,
                std::uint64_t target) {
  struct Step {
    const char* sub;
    void (*forget)(Meta&);
  };
  constexpr std::array<Step, 3> kSteps = {{{"planes", forget_planes}, {"maps", nullptr}, {"frames", forget_frames}}};
  for (const Step& step : kSteps) {
    if (total <= target) {
      return true;
    }
    const std::uint64_t freed = std::min(remove_tree(dir / step.sub), r.bytes);
    if (freed == 0) {
      continue;
    }
    if (step.forget != nullptr) {
      rewrite_meta(dir, step.forget);
    }
    // meta.json may have changed size: re-walk this one entry (eviction is rare, §6.7.4 notes).
    const std::uint64_t now_bytes = directory_bytes(dir);
    total = total - r.bytes + now_bytes;
    r.bytes = now_bytes;
    OVT_LOG(log::Level::Info, kModule, "evicted {}/ of entry {} ({} bytes)", step.sub, hash, freed);
  }
  return total <= target;
}

}  // namespace

// SPEC-ASSUMPTION(#297): the file-to-kind map of `ovt cache list` and `clear --kind` (accepted by the reviewer).
std::optional<std::string_view> kind_of(const fs::path& relative) {
  auto it = relative.begin();
  if (it == relative.end()) {
    return std::nullopt;
  }
  const std::string first = it->string();
  if (relative.extension() == ".tmp") {
    return std::nullopt;
  }
  if (first == "index.fb" || first == "container.fb.zst") {
    return kIndex;
  }
  if (first == "frames" || first == "planes") {
    return kAnalysis;
  }
  if (first == "maps" || first == "metrics") {
    return kMetrics;
  }
  if (first == "reports") {
    return relative.filename() == "metrics.fb.zst" ? kMetrics : kAnalysis;
  }
  return std::nullopt;
}

std::uint64_t total_bytes(const RecordTable& records) noexcept {
  std::uint64_t total = 0;
  for (const auto& [hash, r] : records) {
    total += r.bytes;
  }
  return total;
}

std::uint64_t directory_bytes(const fs::path& dir) {
  std::uint64_t total = 0;
  std::error_code ec;
  const fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    return 0;
  }
  for (const fs::directory_entry& e : it) {
    if (e.is_regular_file(ec)) {
      total += e.file_size(ec);
    }
  }
  return total;
}

// SPEC-ASSUMPTION(#296): triggered above the cap; every step targets 0.9 x cap (accepted by the reviewer).
EvictionResult evict(const fs::path& root, RecordTable& records, const std::set<std::string>& open, std::uint64_t cap) {
  EvictionResult result;
  std::uint64_t total = total_bytes(records);
  result.bytes_before = total;
  if (total <= cap) {
    result.bytes_after = total;
    return result;
  }
  const std::uint64_t target = cap - cap / 10;  // 0.9 x cap
  // Oldest last_used first; the hash breaks ties so that the order is deterministic.
  std::vector<std::pair<std::string, std::string>> order;
  for (const auto& [hash, r] : records) {
    if (!open.contains(hash)) {
      order.emplace_back(r.last_used, hash);
    }
  }
  std::ranges::sort(order);
  for (const auto& [last_used, hash] : order) {
    if (total <= target) {
      break;
    }
    CacheRecord& r = records.at(hash);
    const fs::path dir = root / hash;
    if (trim_entry(dir, hash, r, total, target)) {
      push_key(result.trimmed, hash);
    } else {
      remove_tree(dir);
      total -= r.bytes;
      records.erase(hash);
      push_key(result.removed, hash);
      OVT_LOG(log::Level::Info, kModule, "evicted entry {}", hash);
    }
  }
  result.bytes_after = total;
  result.all_open = total > cap;
  return result;
}

ClearResult clear_entries(const fs::path& root, RecordTable& records, const std::set<std::string>& open,
                          const ClearFilter& filter) {
  if (filter.kind && std::ranges::find(kKinds, std::string_view(*filter.kind)) == kKinds.end()) {
    throw CacheError(OVT_E_INVALID_ARG, "cache: kind must be index, analysis or metrics");
  }
  std::optional<std::string> cutoff;
  if (filter.older_than_days > 0) {
    cutoff = format_iso8601_utc(now() - std::chrono::hours(24) * filter.older_than_days);
  }
  const std::optional<std::string> source = filter.source ? std::optional(hex(*filter.source)) : std::nullopt;
  ClearResult result;
  std::vector<std::string> keys;
  for (const auto& [hash, r] : records) {
    keys.push_back(hash);
  }
  for (const std::string& hash : keys) {
    CacheRecord& r = records.at(hash);
    if ((source && hash != *source) || (cutoff && r.last_used > *cutoff)) {
      continue;
    }
    if (open.contains(hash)) {
      push_key(result.skipped_open, hash);
      continue;
    }
    const fs::path dir = root / hash;
    if (filter.kind) {
      result.bytes_freed += remove_kind(dir, *filter.kind);
      if (!only_meta_left(dir)) {
        r.bytes = directory_bytes(dir);
        push_key(result.trimmed, hash);
        continue;
      }
    }
    result.bytes_freed += remove_tree(dir);
    records.erase(hash);
    push_key(result.removed, hash);
  }
  return result;
}

std::vector<ListedRecord> list_entries(const fs::path& root, RecordTable& records, bool& changed) {
  changed = false;
  std::error_code ec;
  // Directories cache.json missed (a crash between mkdir and the cache.json write) join the table.
  for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
    const std::string name = e.path().filename().string();
    if (e.is_directory(ec) && is_hex64(name) && !records.contains(name)) {
      records[name].last_used = format_iso8601_utc(now());
      changed = true;
    }
  }
  std::vector<ListedRecord> rows;
  std::vector<std::string> gone;
  for (auto& [hash, r] : records) {
    const fs::path dir = root / hash;
    if (!fs::is_directory(dir, ec)) {
      gone.push_back(hash);
      continue;
    }
    std::map<std::string_view, std::uint64_t> by_kind;
    std::uint64_t total = 0;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec)) {
        continue;
      }
      const std::uint64_t size = e.file_size(ec);
      total += size;
      if (const auto kind = kind_of(e.path().lexically_relative(dir))) {
        by_kind[*kind] += size;
      }
    }
    if (r.bytes != total) {
      r.bytes = total;
      changed = true;
    }
    ListedRecord base;
    base.key = key_of(hash).value_or(SourceHash{});
    base.last_used = r.last_used;
    try {
      const Meta meta = parse_meta(read_file(dir / kMetaJson, kMaxJsonBytes));
      base.source_name = meta.source_name.value_or(std::string());
      base.created = meta.created;
      base.last_used = meta.last_used.value_or(meta.created);
    } catch (const CacheError&) {  // listed with what cache.json knows
    }
    for (const auto& [kind, bytes] : by_kind) {  // std::map: kinds sorted
      ListedRecord row = base;
      row.kind = std::string(kind);
      row.size_bytes = bytes;
      rows.push_back(std::move(row));
    }
  }
  for (const std::string& hash : gone) {
    records.erase(hash);
    changed = true;
  }
  return rows;
}

}  // namespace ovt::cache
