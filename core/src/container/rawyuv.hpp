// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Headerless raw YUV opening (chapter 08 §8.6.6, WI-1.229 part a): the `WxH:pix_fmt[:fps]` string and the reader
// that exposes the frames as a SampleTable. The sidecar, filename/size heuristics and ranking are part b.

#ifndef OVT_CONTAINER_RAWYUV_HPP
#define OVT_CONTAINER_RAWYUV_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "container/pix_fmt_grammar.hpp"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::raw {

struct RawFormat {
  uint32_t width = 0;
  uint32_t height = 0;
  PixFmt pix;
  uint32_t fps_num = 25;  // §12.1.5: the frame rate defaults to 25/1
  uint32_t fps_den = 1;
  uint64_t header_bytes = 0;  // the skipped prefix (`--raw-offset`, sidecar `header_bytes`)
  uint64_t frame_bytes = 0;   // derived: frame_bytes(pix, width, height)
};

// `WxH:pix_fmt[:fps]` with fps as `num/den` or a decimal (`29.97` is 30000/1001). nullopt for anything else.
[[nodiscard]] std::optional<RawFormat> parse_format_string(std::string_view text);

// The frame rate part alone: `num/den` or a decimal. Both parts non-zero.
[[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> parse_fps(std::string_view text);

// frame_count = floor((file_size − header_bytes) / frame_bytes), 0 when the header covers the file.
[[nodiscard]] uint64_t frame_count_of(const RawFormat& fmt, uint64_t file_size) noexcept;

struct Diagnostic {
  DiagCode code;
  std::string message;
  uint64_t offset = 0;
};

// The reader for a known format. Never null.
[[nodiscard]] std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, const RawFormat& fmt);

}  // namespace ovt::container::raw

#endif  // OVT_CONTAINER_RAWYUV_HPP
