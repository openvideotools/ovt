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
// Invalidation (§6.7.3 I1–I7) and size-cap eviction (§6.7.4) are WI-0.092. Cache::entry() is the raw access
// of `ovt cache import` and the C object API: it reports a malformed file as CacheError(OVT_E_CORRUPT) and
// deletes nothing. Cache::open_entry() is the access of stream analysis: it applies I1–I4 to the directory
// and switches the entry to invalidating reads (I5, I6), which delete a bad file and report a miss.

#ifndef OVT_CXX_CACHE_CACHE_HPP
#define OVT_CXX_CACHE_CACHE_HPP

#include <array>
#include <chrono>
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
#include "ovt/cxx/model/plane.hpp"
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

/// Parses the opts_json of ovt_cache_open. Null, empty or "null" give the defaults. Throws
/// CacheError(OVT_E_INVALID_ARG) for text that is not a JSON object, a `max_bytes` that is not a
/// non-negative integer or a `read_only` that is not a boolean. Unknown members are ignored.
[[nodiscard]] Options parse_options_json(std::string_view json);

/// The process default root that ovt_cache_set_dir sets; std::nullopt restores the environment/platform rule.
void set_default_root(std::optional<std::filesystem::path> root);

/// The cache root of §6.7.1 in this order: set_default_root(), the OVT_CACHE_DIR environment variable, then
/// the platform default (macOS ~/Library/Caches/org.openvideotools.ovt/, Linux $XDG_CACHE_HOME/ovt/ or
/// ~/.cache/ovt/; Android has no default: the shell calls set_default_root with Context.getCacheDir()/ovt).
/// Throws CacheError(OVT_E_IO) when nothing yields a directory (HOME unset, or Android without a root).
[[nodiscard]] std::filesystem::path default_root();

/// The rule of §6.7.3 that rejected cached data, or None. Each rule is one check function (WI-0.092 step 1);
/// a rejection is logged at info level with to_string() of its reason.
enum class InvalidationReason : std::uint8_t {
  None = 0,
  ForeignSourceHash,  ///< I1: meta.source_hash differs from the directory name
  CacheFormat,        ///< I2: meta.json is missing, unreadable or its cache_format is not 1
  SchemaMajor,        ///< I3: a schemas.*.major differs from the compiled-in major
  Index,              ///< I4: index.fb does not verify, has another schema_version, or its codec differs
  Frame,              ///< I5: a frame file does not decompress or verify, names another decode_index or plugin
  Plane,              ///< I6: a plane file has no matching PlaneRef or the wrong size
  OvtVersion,         ///< I7: meta.ovt_version differs from the running version (informational, no action)
};

/// "I1 source hash", "I2 cache format", ... ("none" for None).
[[nodiscard]] std::string_view to_string(InvalidationReason reason) noexcept;

/// The analysis plugin a stream session selected (§6.7.3 I5): a cached FrameAnalysis written by another
/// plugin id or version is discarded on read.
struct SelectedPlugin {
  std::string plugin_id;
  std::string plugin_version;
  friend bool operator==(const SelectedPlugin&, const SelectedPlugin&) = default;
};

/// What one Cache::enforce_cap() run did (§6.7.4).
struct EvictionResult {
  std::uint64_t bytes_before = 0;
  std::uint64_t bytes_after = 0;
  std::vector<SourceHash> removed;  ///< entries deleted whole, in eviction order
  std::vector<SourceHash> trimmed;  ///< entries that lost planes/, maps/ or frames/ but stayed
  bool all_open = false;            ///< still above the cap because every remaining entry is open (MDL-CCH-0002)
};

/// Filters of Cache::clear(), as `ovt cache clear [--kind k] [--older-than <days>] [--source <path>]` (§12.2.13).
/// An unset member matches everything.
struct ClearFilter {
  /// "index" (index.fb, container.fb.zst), "analysis" (frames/, planes/, reports/ other than metrics) or
  /// "metrics" (maps/, metrics/, reports/metrics.fb.zst): only those files are deleted from matching entries.
  std::optional<std::string> kind;
  /// Entries whose last_used is at least this many days in the past; 0 matches every entry.
  std::uint32_t older_than_days = 0;
  /// The entry of one source (its §6.2.8 source hash).
  std::optional<SourceHash> source;
};

/// What Cache::clear() did.
struct ClearResult {
  std::uint64_t bytes_freed = 0;
  std::vector<SourceHash> removed;       ///< entries deleted whole
  std::vector<SourceHash> trimmed;       ///< entries that lost the files of ClearFilter::kind
  std::vector<SourceHash> skipped_open;  ///< matching entries left alone because they are open
};

/// One row of Cache::list() (`ovt cache list`, §12.2.13): one per entry and kind present in it.
struct ListedRecord {
  SourceHash key{};
  std::string kind;         ///< "index", "analysis" or "metrics"
  std::string source_name;  ///< meta.source_name, or "" when unknown
  std::uint64_t size_bytes = 0;
  std::string created;    ///< ISO-8601 UTC, "" when meta.json is unreadable
  std::string last_used;  ///< ISO-8601 UTC
};

/// The kinds of ClearFilter::kind and ListedRecord::kind; std::nullopt for meta.json and unknown files.
[[nodiscard]] std::optional<std::string_view> kind_of(const std::filesystem::path& relative);

/// The clock of last_used and of the eviction order: system_clock::now(), unless a test fixed it.
[[nodiscard]] std::chrono::system_clock::time_point now();
/// Test hook: fixes now() (std::nullopt restores the system clock). Process-wide.
void set_now_for_testing(std::optional<std::chrono::system_clock::time_point> when);

/// A plane read back from the cache: the bytes and a view onto them (tight stride, §6.7.1 planes/ layout).
struct PlaneData {
  std::vector<std::uint8_t> bytes;
  model::PlaneView view;  ///< view.data == bytes.data(); stride_bytes == width * sample_bytes
};

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
  /// On an entry from Cache::open_entry() a file that fails I5 (including a plugin other than the selected
  /// one) is deleted instead, and the result is std::nullopt (a miss); other frames are untouched.
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> get_frame(std::uint32_t decode_index) const;

  /// planes/<decode_index>/<stage>-<plane>.pl.zst (zstd level 1): exactly height rows of width * sample_bytes
  /// bytes, stride padding dropped. @p view must pass model::check_plane_view; stage must be a PlaneStage.
  void put_plane(std::uint32_t decode_index, fb::PlaneStage stage, std::uint8_t plane, const model::PlaneView& view);
  /// The stored plane with the geometry of the frame's PlaneRef (§6.7.1: dimensions come from the frame's
  /// FrameAnalysis). std::nullopt when the plane file, the frame or a matching PlaneRef is absent;
  /// CacheError(OVT_E_CORRUPT) when the decompressed size differs from height * width * sample_bytes (I6).
  /// On an entry from Cache::open_entry() a plane file without a matching PlaneRef or of the wrong size is
  /// deleted instead (I6), and the result is std::nullopt.
  [[nodiscard]] std::optional<PlaneData> get_plane(std::uint32_t decode_index, fb::PlaneStage stage,
                                                   std::uint8_t plane) const;

  /// reports/<kind>.fb.zst (zstd level 3), an opaque FlatBuffer ("metrics", "hdr", "verify", ...). @p kind
  /// matches [A-Za-z0-9_-]{1,32}, else CacheError(OVT_E_INVALID_ARG).
  // SPEC-ASSUMPTION(#288): §6.7.1 keys MetricsReports by run_id under metrics/, which the kind-only
  // ovt_cache_put_report of §12.3.14 cannot address; reports/<kind> is the file this call writes.
  void put_report(std::string_view kind, std::span<const std::uint8_t> fb);
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> get_report(std::string_view kind) const;

  /// Whether reads apply I5/I6 (set by Cache::open_entry()), and the plugin they compare against.
  [[nodiscard]] bool invalidating() const;
  [[nodiscard]] std::optional<SelectedPlugin> selected_plugin() const;

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
  /// Records a new `size_cap_bytes` in cache.json (the persistent `--max-size` of §12.2.13; 0 restores
  /// kDefaultMaxBytes). Does not evict by itself: call enforce_cap(). CacheError(OVT_E_UNSUPPORTED) when
  /// read-only.
  void set_max_bytes(std::uint64_t max_bytes);

  /// The entry of @p hash, creating its directory and meta.json when absent (read-only: CacheError(OVT_E_IO)
  /// when absent). Stale *.tmp files under the directory are removed (crash recovery, §6.7.2).
  /// CacheError(OVT_E_CORRUPT) when meta.json exists but does not parse.
  [[nodiscard]] std::shared_ptr<Entry> entry(const SourceHash& hash);

  /// The entry of @p hash for a stream session (§6.7.3): when the directory exists and is not open, I1–I4 are
  /// checked and a failing directory is deleted and recreated (I7 is only logged); then the entry reads with
  /// I5/I6 against @p plugin (std::nullopt: no plugin check). @p codec is the stream's codec: an index.fb of
  /// another codec fails I4. On a read-only cache a failing entry throws CacheError(OVT_E_CORRUPT).
  /// @p reason, when given, receives the rule that rejected the old directory (None when it was kept or new).
  [[nodiscard]] std::shared_ptr<Entry> open_entry(const SourceHash& hash, fb::Codec codec,
                                                  std::optional<SelectedPlugin> plugin,
                                                  InvalidationReason* reason = nullptr);

  /// Source hashes of every entry directory listed in cache.json, sorted.
  [[nodiscard]] std::vector<SourceHash> entries() const;

  /// Sum of cache.json `entries[].bytes`, kept incrementally (no directory walk).
  [[nodiscard]] std::uint64_t total_bytes() const;

  /// The eviction of §6.7.4: when the total exceeds max_bytes(), entries that are not open are trimmed in
  /// order of oldest last_used (planes/, then maps/, then frames/, then the whole directory) until the total
  /// is at most 0.9 x max_bytes(). If only open entries remain above the cap, MDL-CCH-0002 is logged once per
  /// Cache. Runs at open() and after every write that pushes the total above the cap; a no-op when read-only.
  EvictionResult enforce_cap();

  /// Deletes the entries (or, with ClearFilter::kind, the files of that kind) that match @p filter; open
  /// entries are skipped. cache.json follows. CacheError(OVT_E_INVALID_ARG) for an unknown kind,
  /// CacheError(OVT_E_UNSUPPORTED) on a read-only cache.
  ClearResult clear(const ClearFilter& filter);

  /// One row per entry and kind (`ovt cache list`), sorted by key and kind. Re-walks every entry directory and
  /// corrects cache.json `bytes` from what it finds (the only walk besides a missing cache.json, §6.7.4 notes).
  [[nodiscard]] std::vector<ListedRecord> list();

  /// Flushes every live entry's meta.json and then cache.json.
  void flush();

  struct Impl;

 private:
  explicit Cache(std::shared_ptr<Impl> impl);
  /// entry() with Impl::open_mu held.
  [[nodiscard]] std::shared_ptr<Entry> entry_locked(const SourceHash& hash);
  std::shared_ptr<Impl> impl_;
};

}  // namespace ovt::cache

#endif  // OVT_CXX_CACHE_CACHE_HPP
