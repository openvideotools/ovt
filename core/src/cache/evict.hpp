// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Size-cap eviction (chapter 06 §6.7.4), `clear` and the re-walking `list` of the cache (WI-0.092). The
// functions work on the cache.json record table; Cache holds its lock around each call and passes the set of
// entries that are open, which are never touched. Internal header.

#ifndef OVT_CORE_CACHE_EVICT_HPP
#define OVT_CORE_CACHE_EVICT_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cache/cache_json.hpp"
#include "ovt/cxx/cache/cache.hpp"

namespace ovt::cache {

/// Hex source hash → cache.json record (the table of Cache::Impl).
using RecordTable = std::map<std::string, CacheRecord>;

/// Sum of the records' bytes.
[[nodiscard]] std::uint64_t total_bytes(const RecordTable& records) noexcept;

/// Bytes of every regular file below @p dir (0 when absent). Used only where §6.7.4 allows a walk.
[[nodiscard]] std::uint64_t directory_bytes(const std::filesystem::path& dir);

/// The eviction loop of §6.7.4 over the entries of @p records under @p root that are not in @p open. Does
/// nothing when the total is at most @p cap. Deleted entries leave @p records; trimmed ones get their new size.
[[nodiscard]] EvictionResult evict(const std::filesystem::path& root, RecordTable& records,
                                   const std::set<std::string>& open, std::uint64_t cap);

/// Cache::clear() over @p records. Throws CacheError(OVT_E_INVALID_ARG) for an unknown kind.
[[nodiscard]] ClearResult clear_entries(const std::filesystem::path& root, RecordTable& records,
                                        const std::set<std::string>& open, const ClearFilter& filter);

/// Cache::list(): re-walks every entry directory under @p root (adding directories cache.json missed and
/// dropping records whose directory is gone) and corrects @p records. @p changed is set when a record moved.
[[nodiscard]] std::vector<ListedRecord> list_entries(const std::filesystem::path& root, RecordTable& records,
                                                     bool& changed);

}  // namespace ovt::cache

#endif  // OVT_CORE_CACHE_EVICT_HPP
