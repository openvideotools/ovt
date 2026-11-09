// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the Y4M reader (chapter 8 §8.6.5 and §8.9.4, WI-1.228). The whole input is the file. Invariants,
// each an abort when violated: no exception escapes; the walk is deterministic; frames are in order, inside the
// file and each carries frame_bytes plane bytes; the tree, SampleTable and RawVideoFormat can be built and agree
// on the frame count; expanding the root terminates.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "container/y4m.hpp"
#include "io/memory_file.hpp"
#include "ovt_container_generated.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    const ovt::io::MemoryFile file(std::span<const uint8_t>(data, size));
    const auto a = ovt::container::y4m::parse(file);
    const auto b = ovt::container::y4m::parse(file);
    require(a.has_value() == b.has_value());
    if (!a) {
      return 0;
    }
    require(a->frames.size() == b->frames.size() && a->diagnostics.size() == b->diagnostics.size());
    uint64_t prev_end = a->header_bytes;
    for (const auto& fr : a->frames) {
      require(fr.header_offset >= prev_end && fr.data_offset > fr.header_offset);
      prev_end = fr.data_offset + a->frame_bytes;
      require(prev_end <= size);
    }

    ovt_status why = OVT_OK;
    auto reader = ovt::container::y4m::make_reader(file, &why);
    require((reader != nullptr) == !a->unsupported_colour);
    if (!reader) {
      require(why == OVT_E_UNSUPPORTED);
      return 0;
    }
    for (size_t i = 0; i <= a->frames.size() / ovt::container::y4m::kChildWindow + 1; ++i) {
      require(reader->expand(0) == OVT_OK);
    }
    const auto tree = reader->tree();
    const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data());
    require(t->root()->Get(0)->children()->size() == a->frames.size());
    require(flatbuffers::GetRoot<ovt::fb::SampleTable>(reader->sample_table(0, 0, 0).data())->count() ==
            a->frames.size());
    require(flatbuffers::GetRoot<ovt::fb::RawVideoFormat>(reader->raw_format().data())->frame_count() ==
            a->frames.size());
  } catch (...) {
    std::abort();
  }
  return 0;
}
