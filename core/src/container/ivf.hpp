// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The IVF reader (chapter 08 §8.6.2, WI-1.225): header and frame walk, the ContainerTree, one TrackInfo and the
// SampleTable. All integers are little-endian; the header `frame_count` is never trusted.

#ifndef OVT_CONTAINER_IVF_HPP
#define OVT_CONTAINER_IVF_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::ivf {

inline constexpr uint32_t kMinHeaderSize = 32;
inline constexpr uint32_t kFrameHeaderSize = 12;
inline constexpr uint32_t kChildWindow = 4096;  // FRM children materialised per expand (§8.6.2)

struct Header {
  uint16_t version = 0;
  uint16_t header_size = 0;  // as written
  uint32_t fourcc = 0;       // first byte of the fourcc in the low byte (little-endian read)
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t timebase_denominator = 0;  // "rate" in libvpx naming
  uint32_t timebase_numerator = 0;    // "scale"
  uint32_t frame_count = 0;           // as written, unreliable
  uint32_t unused = 0;
};

struct Frame {
  uint64_t header_offset = 0;  // offset of the 12-byte frame header
  uint64_t timestamp = 0;
  uint32_t declared_size = 0;  // frame_size as written
  uint32_t size = 0;           // payload bytes present in the file (clamped for a truncated last frame)

  [[nodiscard]] uint64_t payload_offset() const noexcept { return header_offset + kFrameHeaderSize; }
};

struct Diagnostic {
  DiagCode code;
  std::string message;
  uint64_t offset = 0;  // file offset the finding refers to
};

struct File {
  Header header;
  uint64_t file_size = 0;
  uint64_t first_frame_offset = 0;
  std::vector<Frame> frames;  // frame_count_actual == frames.size()
  std::vector<Diagnostic> diagnostics;
};

// Parses the header and walks every frame. nullopt when the file is not IVF (fewer than 32 bytes or no "DKIF"
// signature). Never throws except for an I/O failure of `file` (io::IoError).
[[nodiscard]] std::optional<File> parse(const io::RandomAccessFile& file);

// Whether a temporal unit / frame starts with a key frame, judged from the first bytes of its payload
// (`prefix`, at most a few KiB are needed). AV01, VP90 and VP80 are understood; any other fourcc is false.
[[nodiscard]] bool frame_is_sync(uint32_t fourcc, std::span<const uint8_t> prefix) noexcept;

// timescale and per-timestamp multiplier of §8.6.2: timescale = rate / g, dts = timestamp * (scale / g).
struct Timing {
  uint32_t timescale = 0;
  uint32_t scale = 0;
};
[[nodiscard]] Timing timing_of(const Header& h) noexcept;

// nullptr when `file` is not IVF.
[[nodiscard]] std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file);

}  // namespace ovt::container::ivf

#endif  // OVT_CONTAINER_IVF_HPP
