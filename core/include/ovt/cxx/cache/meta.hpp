// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The cache entry metadata `meta.json` of chapter 06 §6.7.2 (WI-0.091): the record, its reader and its
// writer. The reader validates against docs/spec/cache-meta-1.schema.json with hand-written checks (no JSON
// Schema library, WI-0.091 step 4). C++ helper header (D22): semi-public, no ABI promise, never included from
// a C header.

#ifndef OVT_CXX_CACHE_META_HPP
#define OVT_CXX_CACHE_META_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ovt::cache {

/// `meta.cache_format`: the only value this reader accepts (§6.7.2 `"const": 1`).
inline constexpr std::uint32_t kCacheFormat = 1;

/// One `schemas.<name>` object: the schema major the writer was built with and the SHA-256 of its .fbs text.
struct SchemaRev {
  std::uint32_t major = 1;
  std::string sha256;  ///< 64 lowercase hex digits (tools/schema_hash.py)
  friend bool operator==(const SchemaRev&, const SchemaRev&) = default;
};

/// One `plugins[]` item: the analysis plugin whose FrameAnalysis files exist under frames/.
struct PluginFrames {
  std::string plugin_id;
  std::string plugin_version;
  std::optional<std::uint32_t> plane_stage_mask;
  std::uint64_t frames = 0;  ///< number of frames/*.fa.zst written by this plugin
  friend bool operator==(const PluginFrames&, const PluginFrames&) = default;
};

/// The whole record. Optional members are the schema's non-required properties; they are omitted from the
/// file when unset. `cache_format` is implied (kCacheFormat).
struct Meta {
  std::string source_hash;  ///< 64 lowercase hex digits; equals the entry directory name
  std::optional<std::string> source_name;
  std::optional<std::uint64_t> source_size;
  std::string created;  ///< ISO-8601 UTC, e.g. 2026-11-03T14:05:09Z
  std::optional<std::string> last_used;
  std::string ovt_version;
  SchemaRev ovt_model;
  SchemaRev ovt_container;
  SchemaRev ovt_metrics;
  std::string codec = "Unknown";  ///< H264 | H265 | AV1 | VP9 | H266 | MPEG2 | Unknown
  std::uint64_t frame_count = 0;
  std::optional<bool> index_complete;
  std::vector<PluginFrames> plugins;
  std::optional<bool> planes_cached;
  friend bool operator==(const Meta&, const Meta&) = default;
};

/// Parses and validates a `meta.json` document. Throws CacheError(OVT_E_CORRUPT) naming the first violated
/// rule when the text is not JSON, a required member is missing, a member has the wrong type, a string does
/// not match its pattern or enum, or an integer is negative or too large. Unknown members are ignored
/// (additive evolution within cache_format 1).
[[nodiscard]] Meta parse_meta(std::span<const std::uint8_t> json);
[[nodiscard]] Meta parse_meta(std::string_view json);

/// Serialises @p meta as pretty-printed JSON with the members in the schema's order and a trailing newline.
/// Deterministic for equal records. Throws CacheError(OVT_E_INVALID_ARG) when a string member would not
/// pass parse_meta (so the file written is always readable).
[[nodiscard]] std::string serialize_meta(const Meta& meta);

/// `YYYY-MM-DDThh:mm:ssZ` in UTC, the timestamp format of §6.7.2.
[[nodiscard]] std::string format_iso8601_utc(std::chrono::system_clock::time_point when);

/// True when @p text has the shape of an RFC 3339 `date-time` (`YYYY-MM-DDThh:mm:ss[.frac](Z|±hh:mm)`).
[[nodiscard]] bool is_iso8601_date_time(std::string_view text) noexcept;

/// True when @p text is exactly 64 lowercase hex digits.
[[nodiscard]] bool is_hex64(std::string_view text) noexcept;

}  // namespace ovt::cache

#endif  // OVT_CXX_CACHE_META_HPP
