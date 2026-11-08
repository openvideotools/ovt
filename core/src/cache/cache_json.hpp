// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `<root>/cache.json` document of chapter 06 §6.7.1 (WI-0.091): the size cap and one record per entry
// directory. Internal to the cache module; the parser is a free function so that the fuzz target can reach it.

#ifndef OVT_CORE_CACHE_CACHE_JSON_HPP
#define OVT_CORE_CACHE_CACHE_JSON_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace ovt::cache {

/// One element of cache.json `entries[]`.
struct CacheRecord {
  std::string last_used;  ///< ISO-8601 UTC, or "" when the entry's meta.json had none
  std::uint64_t bytes = 0;
};

/// The parsed document: `size_cap_bytes` and the records by hex source hash (sorted, deterministic).
struct CacheJson {
  std::optional<std::uint64_t> size_cap_bytes;
  std::map<std::string, CacheRecord> records;
};

/// Parses a cache.json document. std::nullopt when @p bytes is not a JSON object with `cache_format` 1 and an
/// `entries` array, or when an element of `entries` is malformed (the caller then rebuilds the document from
/// the entry directories). An optional member of the wrong type is ignored. Never throws (other than
/// std::bad_alloc).
[[nodiscard]] std::optional<CacheJson> parse_cache_json(std::span<const std::uint8_t> bytes);

}  // namespace ovt::cache

#endif  // OVT_CORE_CACHE_CACHE_JSON_HPP
