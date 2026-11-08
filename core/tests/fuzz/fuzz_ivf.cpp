// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the IVF reader (chapter 8 §8.6.2 and §8.9.4, WI-1.225). The whole input is the file. Invariants,
// each an abort when violated: no exception escapes; the walk is deterministic; the frames are contiguous, start
// at first_frame_offset and stay inside the file; the tree and the full SampleTable can be built and agree on the
// frame count; expanding the root until nothing changes terminates.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "../test_support/memory_file.hpp"
#include "container/ivf.hpp"
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
    const ovt::test::MemoryFile file(std::span<const uint8_t>(data, size));
    const auto a = ovt::container::ivf::parse(file);
    const auto b = ovt::container::ivf::parse(file);
    require(a.has_value() == b.has_value());
    if (!a) {
      return 0;
    }
    require(a->frames.size() == b->frames.size() && a->diagnostics.size() == b->diagnostics.size());
    uint64_t end = a->first_frame_offset;
    for (const auto& fr : a->frames) {
      require(fr.header_offset == end);
      require(fr.size <= fr.declared_size);
      end = fr.payload_offset() + fr.size;
      require(end <= size);
    }

    auto reader = ovt::container::ivf::make_reader(file);
    require(reader != nullptr);
    for (size_t i = 0; i <= a->frames.size() / ovt::container::ivf::kChildWindow + 1; ++i) {
      require(reader->expand(0) == OVT_OK);
    }
    const auto tree = reader->tree();
    require(!tree.empty());
    const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data());
    require(t->root()->size() == 1 && t->root()->Get(0)->children()->size() == a->frames.size());
    const auto table = reader->sample_table(0, 0, 0);
    require(!table.empty());
    require(flatbuffers::GetRoot<ovt::fb::SampleTable>(table.data())->count() == a->frames.size());
  } catch (...) {
    std::abort();
  }
  return 0;
}
