// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the HdrMetadata summary (chapter 08 §8.4.6, WI-1.223). The whole input is the file: it is walked and
// every `trak` is summarised, which re-reads its colour, Dolby Vision and configuration boxes and parses the parameter
// sets / sequence header they carry. Invariants, each an abort when violated: no exception escapes; the serialised
// HdrMetadata verifies; the colour image is 44 bytes.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/hdr_summary.h"
#include "io/memory_file.hpp"
#include "ovt_container_generated.h"

namespace {

namespace iso = ovt::container::isobmff;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

// NOLINTNEXTLINE(misc-no-recursion): the walk bounds the depth (kMaxBoxDepth)
void summarise_traks(const std::vector<std::unique_ptr<ovt::fb::BoxNodeT>>& nodes, const ovt::io::MemoryFile& file) {
  for (const auto& n : nodes) {
    if (n->fourcc == "trak") {
      const flatbuffers::DetachedBuffer buf = iso::build_hdr_metadata(*n, 1, file);
      flatbuffers::Verifier v(buf.data(), buf.size());
      require(v.VerifyBuffer<ovt::fb::HdrMetadata>(nullptr));
      const auto* m = flatbuffers::GetRoot<ovt::fb::HdrMetadata>(buf.data());
      require(m->color() != nullptr && m->color()->size() == 44);
    }
    summarise_traks(n->children, file);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    const ovt::io::MemoryFile file(std::span<const uint8_t>(data, size));
    summarise_traks(iso::walk(file).top_level, file);
  } catch (...) {
    std::abort();
  }
  return 0;
}
