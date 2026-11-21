// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The byte service (chapter 08 §8.8, WI-1.234): ovt_container_bytes, ovt_container_box_at,
// ovt_container_stats_json and the internal unit-bytes path behind ovt_stream_unit_bytes. These are the only
// functions through which the GUI, the CLI and the Python package read file bytes (FR-CNT-104). They are
// thread-safe (the documented exception to chapter 12 §12.3.1 rule 4): they take the page-cache mutex or the
// tree lock and nothing else. No exception leaves them (rule 8).

#include "container/byte_service.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "api/buffer.hpp"
#include "container/container.hpp"
#include "io/ovt_file.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt_container_generated.h"

namespace ovt::container {

namespace {

constexpr uint32_t kMaxBytesRequest = 1U << 20;  // ovt_container_bytes: len <= 1 MiB

// The page cache of `c`, or nullptr when the handle has no file.
PageCache* cache_of(ovt_Container* c) {
  if (c->file == nullptr || c->file->file == nullptr) {
    return nullptr;
  }
  return &c->bytes.cache(*c->file->file);
}

// The innermost box of `nodes` (a sorted, non-overlapping sibling list) that contains `offset`, descending into
// children; sets `*depth` to its depth (top level = 0). nullptr when no box contains it.
const fb::BoxNode* innermost(const flatbuffers::Vector<flatbuffers::Offset<fb::BoxNode>>* nodes, uint64_t offset,
                             int* depth) {
  const fb::BoxNode* found = nullptr;
  int level = 0;
  while (nodes != nullptr && !nodes->empty()) {
    // Binary search: the last sibling that starts at or before `offset`.
    std::size_t lo = 0;
    std::size_t hi = nodes->size();
    while (lo < hi) {
      const std::size_t mid = lo + ((hi - lo) / 2);
      const fb::ByteSpan* span = nodes->Get(static_cast<flatbuffers::uoffset_t>(mid))->file_span();
      if (span != nullptr && span->offset() <= offset) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo == 0) {
      break;
    }
    const fb::BoxNode* candidate = nodes->Get(static_cast<flatbuffers::uoffset_t>(lo - 1));
    const fb::ByteSpan* span = candidate->file_span();
    if (span == nullptr || offset - span->offset() >= span->length()) {
      break;
    }
    found = candidate;
    *depth = level++;
    nodes = candidate->children();
  }
  return found;
}

// Releases a buffer made by api::make_buffer; the same delete[] as ovt_buffer_free() (core/src/status.cpp, which
// lives in the shared library and so is not linked into the module objects).
void release(ovt_Buffer& buf) noexcept {
  delete[] buf.data;  // NOLINT(cppcoreguidelines-owning-memory): the allocator of rule 3a is new[] (api/buffer.cpp)
  buf = ovt_Buffer{nullptr, 0};
}

// leb128 of `value`, appended to `out`.
void put_leb128(uint64_t value, std::vector<uint8_t>& out) {
  do {
    auto byte = static_cast<uint8_t>(value & 0x7FU);
    value >>= 7U;
    if (value != 0) {
      byte |= 0x80U;
    }
    out.push_back(byte);
  } while (value != 0);
}

// AV1 §5.3.2: an OBU stored without obu_size gets obu_has_size_field = 1 and a leb128 obu_size (§8.7.4).
std::vector<uint8_t> av1_with_size_field(const std::vector<uint8_t>& stored) {
  const bool has_extension = (stored[0] & 0x04U) != 0;
  const std::size_t header_len = has_extension ? 2U : 1U;
  std::vector<uint8_t> out;
  out.reserve(stored.size() + 10);
  out.push_back(static_cast<uint8_t>(stored[0] | 0x02U));
  if (has_extension) {
    out.push_back(stored[1]);
  }
  put_leb128(stored.size() - header_len, out);
  out.insert(out.end(), stored.begin() + static_cast<std::ptrdiff_t>(header_len), stored.end());
  return out;
}

// Copies `stored` (and its RBSP, for H.26x) into the caller-owned outputs; both or neither are set.
ovt_status hand_out(const std::vector<uint8_t>& stored, UnitCodec codec, ovt_Buffer* out_raw, ovt_Buffer* out_rbsp) {
  ovt_Buffer raw{nullptr, 0};
  ovt_Buffer rbsp{nullptr, 0};
  if (out_raw != nullptr) {
    if (const ovt_status st = api::make_buffer(stored.data(), stored.size(), &raw); st != OVT_OK) {
      return st;
    }
  }
  if (out_rbsp != nullptr) {
    const bool h26x = codec == UnitCodec::H264 || codec == UnitCodec::H265;
    const std::vector<uint8_t> payload = h26x ? rbsp_from_nal(stored).rbsp : std::vector<uint8_t>{};
    if (const ovt_status st = api::make_buffer(payload.data(), payload.size(), &rbsp); st != OVT_OK) {
      release(raw);
      return st;
    }
  }
  if (out_raw != nullptr) {
    *out_raw = raw;
  }
  if (out_rbsp != nullptr) {
    *out_rbsp = rbsp;
  }
  return OVT_OK;
}

}  // namespace

PageCache& ByteService::cache(const io::RandomAccessFile& file) {
  const std::lock_guard lock(create_mutex_);
  if (!cache_) {
    cache_ = std::make_unique<PageCache>(file);
  }
  return *cache_;
}

ovt_status read_unit_bytes(PageCache& cache, UnitCodec codec, UnitLocation where, ovt_Buffer* out_raw,
                           ovt_Buffer* out_rbsp) noexcept {
  for (ovt_Buffer* out : {out_raw, out_rbsp}) {
    if (out != nullptr) {
      *out = ovt_Buffer{nullptr, 0};
    }
  }
  const uint64_t size = cache.file_size();
  if (where.file_offset > size || where.length > size - where.file_offset) {
    return OVT_E_CORRUPT;  // the unit map points outside the file
  }
  const bool needs_bytes = codec == UnitCodec::H264 || codec == UnitCodec::H265 || codec == UnitCodec::Av1;
  if (needs_bytes && where.length == 0) {
    return OVT_E_CORRUPT;
  }
  try {
    std::vector<uint8_t> stored(static_cast<std::size_t>(where.length));
    if (cache.read(where.file_offset, stored.size(), stored.data()) != stored.size()) {
      return OVT_E_IO;
    }
    if (codec == UnitCodec::Av1 && (stored[0] & 0x02U) == 0) {
      if (((stored[0] & 0x04U) != 0) && stored.size() < 2) {
        return OVT_E_CORRUPT;  // extension flag set, extension byte missing
      }
      stored = av1_with_size_field(stored);
    }
    return hand_out(stored, codec, out_raw, out_rbsp);
  } catch (...) {
    return status_of_current_exception();
  }
}

}  // namespace ovt::container

extern "C" {

OVT_API ovt_status ovt_container_bytes(ovt_Container* container, uint64_t offset, uint32_t len, ovt_Buffer* out) {
  if (out != nullptr) {
    *out = ovt_Buffer{nullptr, 0};
  }
  if (container == nullptr || out == nullptr || len > ovt::container::kMaxBytesRequest) {
    return OVT_E_INVALID_ARG;
  }
  ovt::container::PageCache* cache = ovt::container::cache_of(container);
  if (cache == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    std::vector<uint8_t> bytes(len);
    bytes.resize(cache->read(offset, len, bytes.data()));  // clipped at EOF; empty at or after it
    return ovt::api::make_buffer(bytes.data(), bytes.size(), out);
  } catch (...) {
    return ovt::container::status_of_current_exception();
  }
}

OVT_API ovt_status ovt_container_box_at(ovt_Container* container, uint64_t offset, uint64_t* out_box_offset,
                                        int* out_depth) {
  if (container == nullptr || out_box_offset == nullptr || out_depth == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    const std::lock_guard lock(container->bytes.tree_mutex());  // against ovt_container_expand swapping the tree
    ovt::container::refresh_tree(*container);
    if (container->tree.empty()) {
      return OVT_E_INVALID_ARG;
    }
    const auto* tree = flatbuffers::GetRoot<ovt::fb::ContainerTree>(container->tree.data());
    int depth = 0;
    const ovt::fb::BoxNode* box = ovt::container::innermost(tree->root(), offset, &depth);
    if (box == nullptr) {
      return OVT_E_INVALID_ARG;
    }
    *out_box_offset = box->file_span()->offset();
    *out_depth = depth;
    return OVT_OK;
  } catch (...) {
    return ovt::container::status_of_current_exception();
  }
}

// SPEC-ASSUMPTION(#683): implemented here (§8.8 puts the statistics with the page cache); no WI step names it.
OVT_API ovt_status ovt_container_stats_json(ovt_Container* container, ovt_Buffer* out_json) {
  if (out_json != nullptr) {
    *out_json = ovt_Buffer{nullptr, 0};
  }
  if (container == nullptr || out_json == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  ovt::container::PageCache* cache = ovt::container::cache_of(container);
  if (cache == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    const auto s = cache->stats();
    const std::string json = "{\"hits\":" + std::to_string(s.hits) + ",\"misses\":" + std::to_string(s.misses) +
                             ",\"bytes_read\":" + std::to_string(s.bytes_read) + "}";
    return ovt::api::make_buffer(json, out_json);
  } catch (...) {
    return ovt::container::status_of_current_exception();
  }
}

}  // extern "C"
