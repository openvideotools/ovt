// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the raw-YUV text parsers (chapter 8 §8.6.6 and §8.9.4, WI-1.229). The first 4 bytes (big endian,
// times 1000) are a file size; the rest is each of: a `WxH:pix_fmt[:fps]` string, a pix_fmt name, a frame rate,
// a sidecar and a file name for the heuristics.
// Invariants, each an abort when violated: no exception escapes; every accepted format has a non-zero
// frame_bytes and a pix_fmt that parses back to itself; a sidecar written from an accepted format parses back
// to the same format; candidates are consistent with the file size and ranked deterministically.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "container/rawyuv.hpp"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

namespace raw = ovt::container::raw;

bool same(const raw::RawFormat& a, const raw::RawFormat& b) {
  return a.width == b.width && a.height == b.height && a.pix.name == b.pix.name && a.fps_num == b.fps_num &&
         a.fps_den == b.fps_den && a.header_bytes == b.header_bytes && a.frame_bytes == b.frame_bytes &&
         a.color.present == b.color.present && a.color.primaries == b.color.primaries &&
         a.color.transfer == b.color.transfer && a.color.matrix == b.color.matrix &&
         a.color.full_range == b.color.full_range;
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
    const std::string_view all(reinterpret_cast<const char*>(data), size);
    const std::string_view text = size > 4 ? all.substr(4) : std::string_view{};
    if (const auto f = raw::parse_format_string(text)) {
      check_format(*f);
      const auto g = raw::parse_format_string(text);
      require(g.has_value() && same(*f, *g));
    }
    if (const auto p = raw::parse_pix_fmt(text)) {
      require(p->name == text && raw::frame_bytes(*p, 16, 16) != 0);
    }
    if (const auto fps = raw::parse_fps(text)) {
      require(fps->first != 0 && fps->second != 0);
    }
    if (const auto s = raw::parse_sidecar(text)) {
      check_format(*s);
      const auto round = raw::parse_sidecar(raw::sidecar_json(*s));
      require(round.has_value() && same(*s, *round));
    }
    uint64_t file_size = 0;
    for (size_t i = 0; i < size && i < 4; ++i) {
      file_size = (file_size << 8U) | data[i];
    }
    file_size *= 1000;
    const auto a = raw::candidates(text, file_size);
    const auto b = raw::candidates(text, file_size);
    require(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
      check_format(a[i].format);
      require(same(a[i].format, b[i].format) && a[i].frame_count >= 1);
      require(a[i].frame_count == raw::frame_count_of(a[i].format, file_size));
      require(file_size % a[i].format.frame_bytes == 0);
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
