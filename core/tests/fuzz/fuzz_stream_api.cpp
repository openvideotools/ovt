// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the Stream C API (chapter 12 §12.3.8; chapter 14 §14.7.1; WI-1.121): bytes ->
// ovt_container_open() -> ovt_stream_open() -> ovt_stream_build_index(). The first input byte picks the file name
// whose extension is the sniffer's hint, the rest is the file. Whatever the bytes are, a stream that opens must
// build with OVT_OK, and both the caller's buffer and the handle's copy must pass
// ovt::model::validate_stream_index. A crash, a sanitizer report, another status or a validator finding is a
// finding.
//
// Selector (low two bits): 0 = "f.obu", 1 = "f.ivf", 2 = "f.h264", 3 = "f.h265". Bits 2-3 (WI-1.125) set the
// lazy-tree threshold (0, 1, 2 frames, or the default), so most slices are parsed without a tree and stripped
// lazily; ovt_stream_unit_syntax() then re-parses every unit (the first 64) and must give the same bytes twice.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>

#include <flatbuffers/flatbuffers.h>

#include "api/stream_internal.hpp"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/index.h"
#include "ovt/status.h"
#include "ovt_model_generated.h"

namespace {

// WI-1.125: ovt_stream_unit_syntax() re-parses every unit (the first 64) twice with the same bytes, gives a tree
// wherever the index recorded one, and rejects an index past the end.
void check_unit_syntax(ovt_Stream* stream, const ovt::fb::StreamIndex& index) {
  const auto* units = index.units();
  const uint32_t unit_count = units == nullptr ? 0 : units->size();
  for (uint32_t u = 0; u < std::min<uint32_t>(unit_count, 64); ++u) {
    ovt_Buffer a{nullptr, 0};
    ovt_Buffer b{nullptr, 0};
    if (ovt_stream_unit_syntax(stream, u, &a) != OVT_OK || ovt_stream_unit_syntax(stream, u, &b) != OVT_OK) {
      std::abort();
    }
    const std::unique_ptr<const uint8_t[]> owned_a(a.data);  // NOLINT(*-avoid-c-arrays)
    const std::unique_ptr<const uint8_t[]> owned_b(b.data);  // NOLINT(*-avoid-c-arrays)
    if (a.size != b.size || (a.size != 0 && std::memcmp(a.data, b.data, a.size) != 0) ||
        (units->Get(u)->header() != nullptr && a.size == 0)) {
      std::abort();
    }
  }
  ovt_Buffer none{nullptr, 0};
  if (ovt_stream_unit_syntax(stream, unit_count, &none) != OVT_E_INVALID_ARG) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  static constexpr std::array<const char*, 4> kNames = {"f.obu", "f.ivf", "f.h264", "f.h265"};
  const std::span<const uint8_t> bytes(data + 1, size - 1);
  ovt_File file;
  file.file = std::make_unique<ovt::io::MemoryFile>(bytes, std::string(kNames.at(data[0] & 3U)));
  ovt_Container* container = nullptr;
  if (ovt_container_open(&file, &container) != OVT_OK) {
    return 0;
  }
  ovt_Stream* stream = nullptr;
  if (ovt_stream_open(container, 0, &stream) == OVT_OK) {
    const unsigned lazy = (data[0] >> 2U) & 3U;
    if (lazy != 3) {
      stream->lazy_after_frames = lazy;
    }
    ovt_Buffer out{nullptr, 0};
    if (ovt_stream_build_index(stream, nullptr, &out) != OVT_OK) {
      std::abort();
    }
    // The module objects are linked without ovt_buffer_free(); a caller-owned buffer is a new[] array.
    const std::unique_ptr<const uint8_t[]> owned(out.data);  // NOLINT(*-avoid-c-arrays)
    const ovt_Buffer kept = ovt_stream_index(stream);
    if (!ovt::model::validate_stream_index({out.data, out.size}).empty() ||
        !ovt::model::validate_stream_index({kept.data, kept.size}).empty()) {
      std::abort();
    }
    const auto* frames = flatbuffers::GetRoot<ovt::fb::StreamIndex>(out.data)->frames();
    check_unit_syntax(stream, *flatbuffers::GetRoot<ovt::fb::StreamIndex>(out.data));
    const int count = frames == nullptr ? 0 : static_cast<int>(frames->size());
    for (int d = -1; d <= count; ++d) {
      const int rap = ovt_stream_rap_before(stream, d);
      if (rap > d || rap < -1 || ((d < 0 || d >= count) && rap != -1)) {
        std::abort();
      }
    }
    ovt_stream_close(stream);
  }
  ovt_container_close(container);
  return 0;
}
