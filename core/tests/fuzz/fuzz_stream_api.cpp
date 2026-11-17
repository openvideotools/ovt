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
// Selector (low two bits): 0 = "f.obu", 1 = "f.ivf", 2 = "f.h264", 3 = "f.h265".

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>

#include <flatbuffers/flatbuffers.h>

#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/index.h"
#include "ovt/status.h"
#include "ovt_model_generated.h"

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
