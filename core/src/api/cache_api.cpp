// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/cache.h entry points (chapter 12 §12.3.14, chapter 06 §6.7, WI-0.091; ovt_cache_report and
// ovt_cache_clear WI-0.092): thin wrappers around ovt::cache::Cache and Entry. No exception crosses the ABI (§12.3.1
// rule 8): a CacheError becomes its code, std::bad_alloc OVT_E_NOMEM, anything else OVT_E_INTERNAL. Caller-owned
// buffers (rule 3a) are allocated with new[] and released by ovt_buffer_free().

#include "ovt/cache.h"

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ovt/cxx/cache/cache.hpp"
#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/cxx/model/plane.hpp"
#include "ovt/cxx/model/units.hpp"
#include "ovt/decode.h"
#include "ovt/status.h"
#include "ovt_model_generated.h"

struct ovt_Cache {
  std::shared_ptr<ovt::cache::Cache> cache;
};

struct ovt_CacheEntry {
  std::shared_ptr<ovt::cache::Entry> entry;
};

namespace {

// Runs `fn` and turns any exception into an ovt_status.
template <class Fn>
ovt_status guarded(Fn&& fn) noexcept {
  try {
    return std::forward<Fn>(fn)();
  } catch (const ovt::cache::CacheError& e) {
    try {
      OVT_LOG(ovt::log::Level::Debug, "cache", "{}", e.what());
    } catch (...) {  // NOLINT(bugprone-empty-catch): logging must not change the status
    }
    return e.code;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_INTERNAL;
  }
}

void zero(ovt_Buffer* out) noexcept {
  if (out != nullptr) {
    out->data = nullptr;
    out->size = 0;
  }
}

// Hands `bytes` to the caller as a buffer released by ovt_buffer_free() (new[] / delete[], status.cpp).
ovt_status give(std::span<const std::uint8_t> bytes, ovt_Buffer* out) {
  auto* copy = new std::uint8_t[bytes.empty() ? 1 : bytes.size()];  // NOLINT(cppcoreguidelines-owning-memory)
  if (!bytes.empty()) {
    std::memcpy(copy, bytes.data(), bytes.size());
  }
  out->data = copy;
  out->size = bytes.size();
  return OVT_OK;
}

// The stored record, or OVT_E_IO when the cache has none (§12.3.14: "OVT_E_IO when absent").
ovt_status give_optional(const std::optional<std::vector<std::uint8_t>>& bytes, ovt_Buffer* out) {
  if (!bytes) {
    return OVT_E_IO;
  }
  return give(*bytes, out);
}

// Process-wide settings of the default cache (ovt_cache_set_enabled / ovt_cache_set_max_bytes).
struct Defaults {
  std::mutex mu;
  bool enabled = true;
  std::uint64_t max_bytes = ovt::cache::kDefaultMaxBytes;
  bool max_bytes_set = false;  // ovt_cache_set_max_bytes() was called with a non-zero cap
  std::string dir_text;        // the last ovt_cache_dir() answer; owned by the library (rule 3b)
};

Defaults& defaults() {
  static Defaults d;
  return d;
}

// Options of the default cache: the process-wide cap when one was set, else the cap cache.json records (so
// that a default open never overwrites the persistent `ovt cache --max-size`, #294 review).
ovt::cache::Options default_options() {
  Defaults& d = defaults();
  const std::lock_guard<std::mutex> lock(d.mu);
  ovt::cache::Options options;
  if (d.max_bytes_set) {
    options.max_bytes = d.max_bytes;
  }
  return options;
}

bool plane_in_range(std::uint8_t stage, std::uint8_t plane) noexcept {
  return !ovt::model::stage_token(static_cast<ovt::fb::PlaneStage>(stage)).empty() && plane <= 3;
}

ovt::model::SampleFormat format_of(const ovt_PlaneView& v) {
  switch (v.sample_bytes) {
    case 1:
      return ovt::model::SampleFormat::U8;
    case 2:
      return v.is_signed != 0 ? ovt::model::SampleFormat::S16LE : ovt::model::SampleFormat::U16LE;
    case 4:
      return ovt::model::SampleFormat::S32LE;
    default:
      throw ovt::cache::CacheError(OVT_E_INVALID_ARG, "cache: ovt_PlaneView.sample_bytes is not 1, 2 or 4");
  }
}

}  // namespace

extern "C" {

OVT_API ovt_status ovt_cache_set_dir(const char* dir) {
  return guarded([&] {
    if (dir == nullptr) {
      ovt::cache::set_default_root(std::nullopt);
    } else {
      ovt::cache::set_default_root(std::filesystem::path(dir));
    }
    return OVT_OK;
  });
}

OVT_API const char* ovt_cache_dir(void) {
  Defaults& d = defaults();
  const std::lock_guard<std::mutex> lock(d.mu);
  try {
    d.dir_text = ovt::cache::default_root().string();
  } catch (...) {
    d.dir_text.clear();
  }
  return d.dir_text.c_str();
}

OVT_API ovt_status ovt_cache_set_enabled(int enabled) {
  Defaults& d = defaults();
  const std::lock_guard<std::mutex> lock(d.mu);
  d.enabled = enabled != 0;
  return OVT_OK;
}

OVT_API int ovt_cache_enabled(void) {
  Defaults& d = defaults();
  const std::lock_guard<std::mutex> lock(d.mu);
  return d.enabled ? 1 : 0;
}

OVT_API ovt_status ovt_cache_set_max_bytes(uint64_t max_bytes) {
  Defaults& d = defaults();
  const std::lock_guard<std::mutex> lock(d.mu);
  d.max_bytes = max_bytes == 0 ? ovt::cache::kDefaultMaxBytes : max_bytes;
  d.max_bytes_set = max_bytes != 0;
  return OVT_OK;
}

OVT_API uint64_t ovt_cache_max_bytes(void) {
  Defaults& d = defaults();
  const std::lock_guard<std::mutex> lock(d.mu);
  return d.max_bytes;
}

OVT_API ovt_status ovt_cache_open(const char* root, const char* opts_json, ovt_Cache** out) {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  return guarded([&] {
    ovt::cache::Options options;
    if (opts_json != nullptr) {
      options = ovt::cache::parse_options_json(opts_json);
    } else if (root == nullptr) {
      options = default_options();
    }
    std::optional<std::filesystem::path> path;
    if (root != nullptr && *root != '\0') {
      path = std::filesystem::path(root);
    }
    auto handle = std::make_unique<ovt_Cache>();
    handle->cache = ovt::cache::Cache::open(std::move(path), options);
    *out = handle.release();
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_cache_entry(ovt_Cache* cache, const uint8_t source_hash[32], ovt_CacheEntry** out) {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  if (cache == nullptr || source_hash == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    ovt::cache::SourceHash hash{};
    std::memcpy(hash.data(), source_hash, hash.size());
    auto handle = std::make_unique<ovt_CacheEntry>();
    handle->entry = cache->cache->entry(hash);
    *out = handle.release();
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_cache_put_index(ovt_CacheEntry* entry, const uint8_t* stream_index_fb, size_t len) {
  if (entry == nullptr || stream_index_fb == nullptr || len == 0) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    entry->entry->put_index({stream_index_fb, len});
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_cache_get_index(ovt_CacheEntry* entry, ovt_Buffer* out) {
  zero(out);
  if (entry == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] { return give_optional(entry->entry->get_index(), out); });
}

OVT_API ovt_status ovt_cache_put_frame(ovt_CacheEntry* entry, uint32_t decode_index, const uint8_t* frame_analysis_fb,
                                       size_t len) {
  if (entry == nullptr || frame_analysis_fb == nullptr || len == 0) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    entry->entry->put_frame(decode_index, {frame_analysis_fb, len});
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_cache_get_frame(ovt_CacheEntry* entry, uint32_t decode_index, ovt_Buffer* out) {
  zero(out);
  if (entry == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] { return give_optional(entry->entry->get_frame(decode_index), out); });
}

OVT_API ovt_status ovt_cache_put_plane(ovt_CacheEntry* entry, uint32_t decode_index, uint8_t stage, uint8_t plane,
                                       const ovt_PlaneView* view) {
  if (entry == nullptr || view == nullptr || view->struct_size != sizeof(ovt_PlaneView) ||
      !plane_in_range(stage, plane)) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    ovt::model::PlaneView v;
    v.data = view->data;
    v.width = view->width;
    v.height = view->height;
    v.stride_bytes = view->stride_bytes;
    v.bit_depth = view->bit_depth;
    v.sample_bytes = view->sample_bytes;
    v.format = format_of(*view);
    v.plane = plane;
    v.stage = stage;
    entry->entry->put_plane(decode_index, static_cast<ovt::fb::PlaneStage>(stage), plane, v);
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_cache_get_plane(ovt_CacheEntry* entry, uint32_t decode_index, uint8_t stage, uint8_t plane,
                                       ovt_Buffer* out_storage, ovt_PlaneView* out_view) {
  zero(out_storage);
  if (out_view != nullptr) {
    *out_view = ovt_PlaneView{};
    out_view->struct_size = sizeof(ovt_PlaneView);
  }
  if (entry == nullptr || out_storage == nullptr || out_view == nullptr || !plane_in_range(stage, plane)) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    const auto got = entry->entry->get_plane(decode_index, static_cast<ovt::fb::PlaneStage>(stage), plane);
    if (!got) {
      return OVT_E_IO;
    }
    const ovt_status st = give(got->bytes, out_storage);
    if (st != OVT_OK) {
      return st;
    }
    out_view->data = out_storage->data;
    out_view->width = got->view.width;
    out_view->height = got->view.height;
    out_view->stride_bytes = got->view.stride_bytes;
    out_view->bit_depth = got->view.bit_depth;
    out_view->sample_bytes = got->view.sample_bytes;
    out_view->is_signed =
        (got->view.format == ovt::model::SampleFormat::S16LE || got->view.format == ovt::model::SampleFormat::S32LE)
            ? 1
            : 0;
    out_view->plane = plane;
    out_view->stage = stage;
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_cache_put_report(ovt_CacheEntry* entry, const char* kind, const uint8_t* fb, size_t len) {
  if (entry == nullptr || kind == nullptr || fb == nullptr || len == 0) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    entry->entry->put_report(kind, {fb, len});
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_cache_get_report(ovt_CacheEntry* entry, const char* kind, ovt_Buffer* out) {
  zero(out);
  if (entry == nullptr || kind == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] { return give_optional(entry->entry->get_report(kind), out); });
}

OVT_API ovt_status ovt_cache_report(ovt_Buffer* out) {
  zero(out);
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    const std::filesystem::path root = ovt::cache::default_root();
    std::vector<ovt::cache::ListedRecord> rows;
    std::uint64_t total = 0;
    std::uint64_t cap = ovt_cache_max_bytes();
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
      ovt::cache::Options read_only;  // a query: nothing is created, written or evicted
      read_only.read_only = true;
      const auto cache = ovt::cache::Cache::open(root, read_only);
      rows = cache->list();
      total = cache->total_bytes();
      cap = cache->max_bytes();
    }
    const ovt::cache::LookupCounters counters = ovt::cache::lookup_counters();
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<ovt::fb::CacheEntry>> entries;
    entries.reserve(rows.size());
    for (const ovt::cache::ListedRecord& r : rows) {
      entries.push_back(ovt::fb::CreateCacheEntryDirect(b, ovt::cache::hex(r.key).c_str(), r.kind.c_str(),
                                                        r.source_name.c_str(), r.size_bytes, r.created.c_str(),
                                                        r.last_used.c_str()));
    }
    b.Finish(ovt::fb::CreateCacheReportDirect(b, root.string().c_str(), &entries, total, cap, counters.hits,
                                              counters.misses));
    return give({b.GetBufferPointer(), b.GetSize()}, out);
  });
}

OVT_API ovt_status ovt_cache_clear(const char* kind, const char* source_path, int older_than_days) {
  if (older_than_days < 0) {
    return OVT_E_INVALID_ARG;
  }
  if (kind != nullptr) {
    const std::string_view k(kind);
    if (k != "index" && k != "analysis" && k != "metrics") {
      return OVT_E_INVALID_ARG;
    }
  }
  if (source_path != nullptr) {
    // SPEC-ASSUMPTION(#300): the §6.2.8 source hasher arrives with the stream index (WI-1.121); until then a
    // source cannot be mapped to its entry.
    return OVT_E_UNSUPPORTED;
  }
  return guarded([&] {
    const std::filesystem::path root = ovt::cache::default_root();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
      return OVT_OK;
    }
    ovt::cache::ClearFilter filter;
    if (kind != nullptr) {
      filter.kind = std::string(kind);
    }
    filter.older_than_days = static_cast<std::uint32_t>(older_than_days);
    (void)ovt::cache::Cache::open(root, default_options())->clear(filter);
    return OVT_OK;
  });
}

OVT_API void ovt_cache_entry_release(ovt_CacheEntry* entry) {
  if (entry == nullptr) {
    return;
  }
  (void)guarded([&] {
    entry->entry->flush();
    return OVT_OK;
  });
  delete entry;  // NOLINT(cppcoreguidelines-owning-memory): C handle
}

OVT_API void ovt_cache_close(ovt_Cache* cache) {
  if (cache == nullptr) {
    return;
  }
  (void)guarded([&] {
    cache->cache->flush();
    return OVT_OK;
  });
  delete cache;  // NOLINT(cppcoreguidelines-owning-memory): C handle
}

}  // extern "C"
