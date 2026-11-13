// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/cache.h
 * @brief The on-disk analysis cache (chapter 12 §12.3.14, chapter 06 §6.7, WI-0.091).
 *
 * Two layers. The process-wide functions (ovt_cache_set_dir, ovt_cache_dir, ovt_cache_set_enabled,
 * ovt_cache_set_max_bytes) configure the default cache that stream analysis uses. The object API
 * (ovt_cache_open and the ovt_CacheEntry functions) addresses one cache root explicitly; `ovt cache import`
 * and the tests use it. Files and names are exactly those of chapter 06 §6.7.1: `<root>/cache.json`,
 * `<root>/<source_hash hex>/meta.json`, `index.fb`, `frames/%08u.fa.zst`, `planes/%08u/<stage>-<plane>.pl.zst`.
 *
 * Buffers are stored size-prefixed in the cache but cross this API without a size prefix (§6.1.1): a caller
 * puts the bytes of a plain `FinishBuffer` and gets the same bytes back.
 *
 * Size-cap eviction (§6.7.4, WI-0.092) runs when a cache opens and after every write that pushes it above its
 * cap; it only removes entries that no handle holds. The object API is raw access: a malformed file is
 * reported as OVT_E_CORRUPT and never deleted (the invalidation rules of §6.7.3 apply to stream sessions).
 * ovt_cache_report() and ovt_cache_clear() serve `ovt cache` (§12.2.13) on the default cache.
 *
 * Threading (§12.3.1 rule 4): an ovt_Cache and its ovt_CacheEntry handles may be used from several threads
 * at once; every function takes the handle's own lock.
 */
#ifndef OVT_CACHE_H
#define OVT_CACHE_H

#include <stddef.h> /* NOLINT(modernize-deprecated-headers): a C11 header */
#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#include "ovt/decode.h"
#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using): a C11 header */
/** One cache root. Opaque; released with ovt_cache_close(). */
typedef struct ovt_Cache ovt_Cache;
/** One `<root>/<source_hash>/` directory. Opaque; released with ovt_cache_entry_release(). */
typedef struct ovt_CacheEntry ovt_CacheEntry;
/* NOLINTEND(modernize-use-using) */

/**
 * Sets the root of the default cache for this process (UTF-8 path, copied). NULL restores the §6.7.1 rule:
 * the OVT_CACHE_DIR environment variable, else the platform default (Linux `$XDG_CACHE_HOME/ovt` or
 * `~/.cache/ovt`, macOS `~/Library/Caches/org.openvideotools.ovt`; Android has no default and the shell
 * passes `Context.getCacheDir()/ovt`). Call it before any stream is opened; later calls affect only caches
 * opened afterwards. Never fails for a non-NULL path; it is not created until a cache opens.
 */
OVT_API ovt_status ovt_cache_set_dir(const char* dir);

/**
 * The root the default cache would use now (UTF-8). Owned by the library (§12.3.1 rule 3b): valid until the
 * next ovt_cache_set_dir() or ovt_cache_dir() call on any thread; MUST NOT be freed. "" when the rule yields
 * no directory (HOME unset, or Android without ovt_cache_set_dir()).
 */
OVT_API const char* ovt_cache_dir(void);

/** Enables (1) or disables (0) the default cache for streams opened afterwards. Enabled by default. */
OVT_API ovt_status ovt_cache_set_enabled(int enabled);

/** Whether the default cache is enabled: 1 or 0. */
OVT_API int ovt_cache_enabled(void);

/**
 * Size cap of the default cache in bytes (chapter 06 §6.7.4, NFR-010): default 20 GiB on desktop, 2 GiB
 * on Android. Recorded in `cache.json` by the next ovt_cache_open() of the default root, which evicts down to
 * it. 0 withdraws the setting: the default cache then keeps the cap its `cache.json` records.
 */
OVT_API ovt_status ovt_cache_set_max_bytes(uint64_t max_bytes);

/** The cap set by ovt_cache_set_max_bytes(), or the platform default when none is set. */
OVT_API uint64_t ovt_cache_max_bytes(void);

/**
 * Opens the cache at @p root (UTF-8; NULL = the default root of ovt_cache_dir()), creating the directory
 * with mode 0700 when absent. @p opts_json is NULL or a JSON object `{"max_bytes": n, "read_only": false}`
 * (D4 d: options are a JSON string, not a struct); unknown members are ignored; a NULL @p opts_json for the
 * default root takes the cap of ovt_cache_set_max_bytes() when one is set, else the cap `cache.json` records
 * (the platform default for a new cache). Opening evicts down to the cap. On success *@p out receives a handle the
 * caller releases with ovt_cache_close().
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL @p out, an @p opts_json that is not a JSON object or has a
 *         member of the wrong type; OVT_E_IO when the root cannot be created or read, or when no default
 *         root exists; OVT_E_CORRUPT when `cache.json` cannot be rebuilt. *@p out is NULL on failure.
 */
OVT_API ovt_status ovt_cache_open(const char* root, const char* opts_json, ovt_Cache** out);

/**
 * The entry of @p source_hash (32 bytes, chapter 06 §6.7.1 source hash), creating `<root>/<hex>/` and a
 * fresh `meta.json` when absent (unless the cache is read-only: OVT_E_IO). Two calls with the same hash on
 * one ovt_Cache share the entry; each handle is released once with ovt_cache_entry_release(). On success
 * *@p out receives the handle.
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle, hash or @p out; OVT_E_CORRUPT when the existing
 *         `meta.json` or `index.fb` is malformed or names another source; OVT_E_IO on a file-system failure.
 *         *@p out is NULL on failure.
 */
OVT_API ovt_status ovt_cache_entry(ovt_Cache* cache, const uint8_t source_hash[32], ovt_CacheEntry** out);

/**
 * Stores the StreamIndex @p stream_index_fb (@p len bytes, root type `ovt.fb.StreamIndex`, identifier
 * "OVTI", not size-prefixed; its `source_hash` must equal the entry's) as `index.fb` and maps it. The bytes
 * are copied; the caller keeps ownership of @p stream_index_fb.
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle or buffer, a buffer that does not verify, or another
 *         source hash; OVT_E_UNSUPPORTED on a read-only cache; OVT_E_IO on a file-system failure.
 */
OVT_API ovt_status ovt_cache_put_index(ovt_CacheEntry* entry, const uint8_t* stream_index_fb, size_t len);

/**
 * The stored StreamIndex, as put (no size prefix). *@p out is owned by the caller and released with
 * ovt_buffer_free() (§12.3.1 rule 3a).
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle or @p out; OVT_E_IO when the entry has no index;
 *         OVT_E_CORRUPT when `index.fb` does not verify. *@p out is zeroed on failure.
 */
OVT_API ovt_status ovt_cache_get_index(ovt_CacheEntry* entry, ovt_Buffer* out);

/**
 * Stores the FrameAnalysis @p frame_analysis_fb (@p len bytes, identifier "OVTA", not size-prefixed, its
 * `frame.id.decode_index` equal to @p decode_index) as `frames/%08u.fa.zst` (zstd level 3). One record per
 * decode_index (§6.7.2): a record from another plugin is replaced and `meta.plugins` adjusted. The bytes are
 * copied; the caller keeps ownership.
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle or buffer, a buffer that does not verify, or a
 *         decode_index that differs from the record's; OVT_E_UNSUPPORTED on a read-only cache; OVT_E_IO on
 *         a file-system failure.
 */
OVT_API ovt_status ovt_cache_put_frame(ovt_CacheEntry* entry, uint32_t decode_index, const uint8_t* frame_analysis_fb,
                                       size_t len);

/**
 * The stored FrameAnalysis of @p decode_index, as put. *@p out is owned by the caller and released with
 * ovt_buffer_free().
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle or @p out; OVT_E_IO when no record exists;
 *         OVT_E_CORRUPT when the file does not decompress or verify, or holds another decode_index. *@p out
 *         is zeroed on failure.
 */
OVT_API ovt_status ovt_cache_get_frame(ovt_CacheEntry* entry, uint32_t decode_index, ovt_Buffer* out);

/**
 * Stores the plane @p view (chapter 12 §12.3.9 layout: rows `stride_bytes` apart) of frame @p decode_index,
 * @p stage (an `ovt.fb.PlaneStage` value) and @p plane (0 Y, 1 U, 2 V, 3 alpha) as
 * `planes/%08u/<stage>-<plane>.pl.zst`: exactly `height` rows of `width * sample_bytes` bytes, zstd level 1.
 * The samples are copied; the caller keeps ownership of @p view and its memory. @p view->struct_size must be
 * sizeof(ovt_PlaneView).
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle or view, a stage or plane outside the enum, or a view
 *         that fails the §6.6.1 rules (NULL data with a size, stride below width * sample_bytes, sample_bytes
 *         not 1, 2 or 4); OVT_E_UNSUPPORTED on a read-only cache; OVT_E_IO on a file-system failure.
 */
OVT_API ovt_status ovt_cache_put_plane(ovt_CacheEntry* entry, uint32_t decode_index, uint8_t stage, uint8_t plane,
                                       const ovt_PlaneView* view);

/**
 * The stored plane. The geometry comes from the frame's `FrameAnalysis.planes` PlaneRef (§6.7.1), so the
 * frame must be in the cache. *@p out_storage receives the tight samples (owned by the caller, released
 * with ovt_buffer_free()) and *@p out_view a view onto them with `stride_bytes == width * sample_bytes`.
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle or out-parameter, or a stage or plane outside the
 *         enum; OVT_E_IO when the plane file, the frame or its PlaneRef is absent; OVT_E_CORRUPT when the
 *         file does not decompress or its size differs from height * width * sample_bytes (I6). Both
 *         out-parameters are zeroed on failure.
 */
OVT_API ovt_status ovt_cache_get_plane(ovt_CacheEntry* entry, uint32_t decode_index, uint8_t stage, uint8_t plane,
                                       ovt_Buffer* out_storage, ovt_PlaneView* out_view);

/**
 * Stores the opaque FlatBuffer @p fb (@p len bytes) of @p kind ("metrics", "hdr", "verify", ...; matches
 * `[A-Za-z0-9_-]{1,32}`) as `reports/<kind>.fb.zst` (zstd level 3), replacing a previous one. The bytes are
 * copied; the caller keeps ownership.
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle, kind or buffer, an empty buffer or a kind outside the
 *         pattern; OVT_E_UNSUPPORTED on a read-only cache; OVT_E_IO on a file-system failure.
 */
OVT_API ovt_status ovt_cache_put_report(ovt_CacheEntry* entry, const char* kind, const uint8_t* fb, size_t len);

/**
 * The stored report of @p kind, as put. *@p out is owned by the caller and released with ovt_buffer_free().
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL handle, kind or @p out, or a kind outside the pattern;
 *         OVT_E_IO when no report of that kind exists; OVT_E_CORRUPT when the file does not decompress.
 *         *@p out is zeroed on failure.
 */
OVT_API ovt_status ovt_cache_get_report(ovt_CacheEntry* entry, const char* kind, ovt_Buffer* out);

/**
 * A `CacheReport` (chapter 06 §6.3, `ovt_model.fbs`; root table without file identifier, not size-prefixed)
 * of the default cache: `dir` = ovt_cache_dir(); one `CacheEntry` per entry and kind present ("index",
 * "analysis", "metrics"; `key` = source hash hex, `source_basename`, `size_bytes`, `created`, `last_used`),
 * sorted by key and kind; `total_bytes`; `max_bytes` = the cap `cache.json` records; `hits` and `misses` =
 * cache lookups (get_index, get_frame, get_plane, get_report) answered or missed by this process. Every
 * entry directory is re-walked, so `size_bytes` is exact. Nothing is created or written: a root that does
 * not exist yet gives an empty report. *@p out is owned by the caller and released with ovt_buffer_free().
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL @p out; OVT_E_IO when no default root exists or it cannot be
 *         read. *@p out is zeroed on failure.
 */
OVT_API ovt_status ovt_cache_report(ovt_Buffer* out);

/**
 * Deletes entries of the default cache, as `ovt cache clear` (§12.2.13). Filters combine; NULL / 0 match
 * everything. @p kind ("index", "analysis" or "metrics") deletes only the files of that kind from matching
 * entries, and an entry left with nothing else is removed. @p older_than_days > 0 matches entries last used
 * at least that many days ago. @p source_path selects the entry of one source file (its §6.2.8 source hash).
 * Entries open in this process are skipped. A root that does not exist yet is left uncreated.
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for an unknown @p kind or a negative @p older_than_days;
 *         OVT_E_UNSUPPORTED for a non-NULL @p source_path until the source hasher of chapter 06 §6.2.8 is in
 *         the library; OVT_E_IO when no default root exists or a file-system call fails.
 */
OVT_API ovt_status ovt_cache_clear(const char* kind, const char* source_path, int older_than_days);

/**
 * Releases @p entry (NULL is ignored), writing `meta.json` when it changed and the entry's `cache.json`
 * record. The directory stays on disk.
 */
OVT_API void ovt_cache_entry_release(ovt_CacheEntry* entry);

/**
 * Releases @p cache (NULL is ignored), writing `cache.json` when it changed. Entries obtained from it must
 * have been released first; an entry still alive keeps the shared state until its own release.
 */
OVT_API void ovt_cache_close(ovt_Cache* cache);

#ifdef __cplusplus
}
#endif

#endif /* OVT_CACHE_H */
