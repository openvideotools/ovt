// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::cache::Cache, the on-disk analysis cache of chapter 06 §6.7 (WI-0.091): one directory per source
// hash with meta.json, the mmap-ed index.fb, zstd-compressed FrameAnalysis, plane and report files, and the
// cache-wide cache.json. The C API of ovt/cache.h (chapter 12 §12.3.14) is a thin wrapper over this class.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.
//
// Buffers cross this interface the way they cross the C ABI (§6.1.1): a FlatBuffer with its file identifier
// and WITHOUT a size prefix. The cache adds the 4-byte prefix when it stores a record and strips it when it
// reads one back, so put/get round trips are byte-identical.
//
// Invalidation (§6.7.3 I1–I7) and size-cap eviction (§6.7.4) are WI-0.092: this class reports a malformed
// file as CacheError(OVT_E_CORRUPT) and never deletes an entry on its own.

#ifndef OVT_CXX_CACHE_CACHE_HPP
#define OVT_CXX_CACHE_CACHE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt_model_generated.h"

namespace ovt::cache {

/// `StreamIndex.source_hash` (§6.2.8): the key of an entry.
using SourceHash = std::array<std::uint8_t, 32>;

/// Lowercase hex of @p hash, the entry directory name.
[[nodiscard]] std::string hex(const SourceHash& hash);

/// Inverse of hex(); std::nullopt unless @p text is 64 hex digits (either case).
[[nodiscard]] std::optional<SourceHash> parse_hex(std::string_view text) noexcept;

/// Options of Cache::open; the C API passes them as the JSON object {"max_bytes": n, "read_only": false}.
struct Options {
  /// Size cap recorded in cache.json (`size_cap_bytes`, §6.7.4); the value already in cache.json, else
  /// kDefaultMaxBytes, when unset. Enforced by WI-0.092.
  std::optional<std::uint64_t> max_bytes;
  /// Reads only: no directory is created, no file written, and every put throws CacheError(OVT_E_UNSUPPORTED).
  bool read_only = false;
  friend bool operator==(const Options&, const Options&) = default;
};

/// 20 GiB on desktop, 2 GiB on Android (§6.7.4, NFR-010).
#if defined(__ANDROID__)
inline constexpr std::uint64_t kDefaultMaxBytes = 2ULL << 30U;
#else
inline constexpr std::uint64_t kDefaultMaxBytes = 20ULL << 30U;
#endif

/// The process default root that ovt_cache_set_dir sets; std::nullopt restores the environment/platform rule.
void set_default_root(std::optional<std::filesystem::path> root);

/// The cache root of §6.7.1 in this order: set_default_root(), the OVT_CACHE_DIR environment variable, then
/// the platform default (macOS ~/Library/Caches/org.openvideotools.ovt/, Linux $XDG_CACHE_HOME/ovt/ or
/// ~/.cache/ovt/; Android has no default: the shell calls set_default_root with Context.getCacheDir()/ovt).
/// Throws CacheError(OVT_E_IO) when nothing yields a directory (HOME unset, or Android without a root).
[[nodiscard]] std::filesystem::path default_root();

class Cache;

/// One `<cache_root>/<source_hash>/` directory (§6.7.1). Obtained from Cache::entry(); the same source hash
/// always yields the same Entry object while one is alive, so handles share its metadata.
///
/// Thread-safety: all methods may be called concurrently; writes to one entry are serialised, reads run in
/// parallel with each other and with writes (a reader sees either the previous or the new file, never a
/// partial one, because every file is written to a temporary name and renamed).
class Entry {
 public:
  ~Entry();
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;
  Entry(Entry&&) = delete;
  Entry& operator=(Entry&&) = delete;

  [[nodiscard]] const SourceHash& source_hash() const noexcept { return hash_; }
  [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }
  /// A snapshot of meta.json as it will be written at the next flush.
  [[nodiscard]] Meta meta() const;
  /// Bytes of every file under dir(), kept incrementally (cache.json `entries[].bytes`).
  [[nodiscard]] std::uint64_t bytes() const noexcept;

  /// Records the informational `source_name` / `source_size` members of meta.json (never an absolute path).
  void set_source_info(std::string_view name, std::uint64_t size);

  /// index.fb. @p stream_index is a StreamIndex FlatBuffer with identifier "OVTI" whose source_hash equals
  /// source_hash(); meta.codec, meta.frame_count and meta.index_complete follow from it. Throws
  /// CacheError(OVT_E_INVALID_ARG) for any other buffer.
  void put_index(std::span<const std::uint8_t> stream_index);
  /// The stored StreamIndex, or std::nullopt when absent. The file was verified when mapped; a corrupt file
  /// throws CacheError(OVT_E_CORRUPT).
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> get_index() const;

  /// frames/<decode_index 8 digits>.fa.zst (zstd level 3). @p frame_analysis is a FrameAnalysis FlatBuffer
  /// ("OVTA") whose frame.id.decode_index equals @p decode_index (I5), else CacheError(OVT_E_INVALID_ARG).
  /// Only one FrameAnalysis per decode_index is kept (§6.7.2): a file written by another plugin is replaced
  /// and meta.plugins is adjusted.
  void put_frame(std::uint32_t decode_index, std::span<const std::uint8_t> frame_analysis);
  /// The stored FrameAnalysis, or std::nullopt when absent; CacheError(OVT_E_CORRUPT) when the file does not
  /// decompress or verify, or names another decode_index.
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> get_frame(std::uint32_t decode_index) const;

  /// Writes meta.json atomically when it changed (write meta.json.tmp, fsync, rename; §6.7.2). Called by the
  /// destructor and by Cache::flush().
  void flush();

  struct Impl;

 private:
  friend class Cache;
  Entry(std::shared_ptr<Impl> impl, SourceHash hash, std::filesystem::path dir);

  std::shared_ptr<Impl> impl_;
  SourceHash hash_;
  std::filesystem::path dir_;
};

/// One cache root (§6.7.1). Open with Cache::open(); entries are obtained with entry(). Releasing the last
/// reference flushes cache.json; entries may outlive the Cache object (they keep what they need alive).
class Cache {
 public:
  /// Opens (creating when needed, mode 0700 for a new root) the cache at @p root, or at default_root() when
  /// @p root is std::nullopt. Reads cache.json, or rebuilds it from the entry directories when it is missing
  /// or malformed. Throws CacheError(OVT_E_IO) when the root cannot be created or is not a directory.
  [[nodiscard]] static std::shared_ptr<Cache> open(std::optional<std::filesystem::path> root, Options options = {});
  ~Cache();
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;
  Cache(Cache&&) = delete;
  Cache& operator=(Cache&&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] const Options& options() const noexcept;
  /// `size_cap_bytes` as cache.json records it.
  [[nodiscard]] std::uint64_t max_bytes() const noexcept;

  /// The entry of @p hash, creating its directory and meta.json when absent (read-only: CacheError(OVT_E_IO)
  /// when absent). Stale *.tmp files under the directory are removed (crash recovery, §6.7.2).
  /// CacheError(OVT_E_CORRUPT) when meta.json exists but does not parse.
  [[nodiscard]] std::shared_ptr<Entry> entry(const SourceHash& hash);

  /// Source hashes of every entry directory listed in cache.json, sorted.
  [[nodiscard]] std::vector<SourceHash> entries() const;

  /// Flushes every live entry's meta.json and then cache.json.
  void flush();

  struct Impl;

 private:
  explicit Cache(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

}  // namespace ovt::cache

#endif  // OVT_CXX_CACHE_CACHE_HPP
