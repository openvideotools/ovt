// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The Y4M (YUV4MPEG2) reader (chapter 08 §8.6.5, WI-1.228): header tags, the `C` colour-space table, the FRAME
// walk, the ContainerTree, one TrackInfo, the SampleTable and the RawVideoFormat.

#ifndef OVT_CONTAINER_Y4M_HPP
#define OVT_CONTAINER_Y4M_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::y4m {

inline constexpr size_t kMaxHeaderLine = size_t{64} * 1024;  // header line cap (§8.6.5 pitfalls)
inline constexpr size_t kMaxFrameLine = 4096;                // FRAME line cap
inline constexpr uint32_t kChildWindow = 4096;               // FRAME children materialised per expand

// What a `C` value means (§8.6.5 table).
struct ColourSpace {
  uint8_t chroma_format = 1;  // 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4, 4 = 4:1:1 (SPEC-ASSUMPTION(#280))
  uint8_t bit_depth = 8;
  uint8_t siting = 2;   // ovt_ColorDescription.chroma_siting; 0 = unknown / not applicable
  bool alpha = false;   // 444alpha: a fourth full-size plane
  std::string pix_fmt;  // FFmpeg name, e.g. "yuv420p10le"
};

// nullopt for a `C` value that is not in the table (CNT-Y4M-0002).
[[nodiscard]] std::optional<ColourSpace> colour_space(std::string_view c);

// Bytes of one frame's planes (no FRAME line). Zero when the arithmetic overflows.
[[nodiscard]] uint64_t frame_bytes(const ColourSpace& cs, uint32_t width, uint32_t height) noexcept;

struct Tag {
  std::string text;     // the whole token, e.g. "F30000:1001"
  uint64_t offset = 0;  // file offset of the token
};

struct Frame {
  uint64_t header_offset = 0;  // offset of "FRAME"
  uint64_t data_offset = 0;    // first plane byte
  std::vector<Tag> tags;       // frame-level tags (e.g. "Ip")
};

struct Diagnostic {
  DiagCode code;
  std::string message;
  uint64_t offset = 0;
};

struct File {
  uint64_t file_size = 0;
  uint64_t header_bytes = 0;     // length of the stream header line including the newline
  std::vector<Tag> header_tags;  // every token after the signature, in order (unknown tags kept)
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_num = 25;
  uint32_t fps_den = 1;
  char interlacing = '?';
  uint32_t aspect_num = 0;
  uint32_t aspect_den = 0;
  std::string c_value = "420jpeg";
  ColourSpace colour;
  bool full_range = false;  // XCOLORRANGE=FULL
  uint64_t frame_bytes = 0;
  std::vector<Frame> frames;
  std::vector<Diagnostic> diagnostics;
  bool unsupported_colour = false;  // CNT-Y4M-0002: open fails with OVT_E_UNSUPPORTED
};

// nullopt when the bytes are not a usable Y4M stream header (no signature, no newline within 64 KiB, missing or
// invalid W/H). A stream with an unknown `C` value parses (unsupported_colour set, no frames walked).
// Never throws except for an I/O failure of `file` (io::IoError).
[[nodiscard]] std::optional<File> parse(const io::RandomAccessFile& file);

// nullptr when `file` is not Y4M; *why (if given) is OVT_E_CORRUPT or OVT_E_UNSUPPORTED (unknown `C`).
[[nodiscard]] std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, ovt_status* why = nullptr);

}  // namespace ovt::container::y4m

#endif  // OVT_CONTAINER_Y4M_HPP
