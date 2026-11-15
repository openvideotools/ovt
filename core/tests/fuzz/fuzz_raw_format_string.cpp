// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the raw-YUV text parsers (chapter 8 §8.6.6 and §8.9.4, WI-1.229 part a). The whole input is
// each of: a `WxH:pix_fmt[:fps]` string, a pix_fmt name and a frame rate. Invariants, each an abort when violated:
// no exception escapes; every accepted format has a non-zero frame_bytes and a pix_fmt that parses back to itself;
// parsing is deterministic; an accepted format builds a reader whose RawVideoFormat and SampleTable agree.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <flatbuffers/flatbuffers.h>

#include <span>

#include "container/rawyuv.hpp"
#include "io/memory_file.hpp"
#include "ovt_container_generated.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

namespace raw = ovt::container::raw;

bool same(const raw::RawFormat& a, const raw::RawFormat& b) {
  return a.width == b.width && a.height == b.height && a.pix.name == b.pix.name && a.fps_num == b.fps_num &&
         a.fps_den == b.fps_den && a.header_bytes == b.header_bytes && a.frame_bytes == b.frame_bytes;
}

void check_format(const raw::RawFormat& f) {
  require(f.width >= 1 && f.height >= 1 && f.width <= raw::kMaxDimension && f.height <= raw::kMaxDimension);
  require(f.frame_bytes != 0 && f.frame_bytes == raw::frame_bytes(f.pix, f.width, f.height));
  require(f.fps_num != 0 && f.fps_den != 0);
  const auto again = raw::parse_pix_fmt(f.pix.name);
  require(again.has_value() && again->name == f.pix.name && again->bit_depth == f.pix.bit_depth);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const auto f = raw::parse_format_string(text)) {
      check_format(*f);
      const auto g = raw::parse_format_string(text);
      require(g.has_value() && same(*f, *g));
      const ovt::io::MemoryFile file(std::span<const uint8_t>(data, size));
      const auto reader = raw::make_reader(file, *f);
      const auto rf = reader->raw_format();
      const auto count = flatbuffers::GetRoot<ovt::fb::RawVideoFormat>(rf.data())->frame_count();
      require(count == raw::frame_count_of(*f, size));
      const auto table = reader->sample_table(0, 0, 0);
      require(flatbuffers::GetRoot<ovt::fb::SampleTable>(table.data())->count() == count);
    }
    if (const auto p = raw::parse_pix_fmt(text)) {
      require(p->name == text && raw::frame_bytes(*p, 16, 16) != 0);
    }
    if (const auto fps = raw::parse_fps(text)) {
      require(fps->first != 0 && fps->second != 0);
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
