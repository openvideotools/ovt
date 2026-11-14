// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Headerless raw YUV opening (chapter 08 §8.6.6, WI-1.229): the `WxH:pix_fmt[:fps]` string, the
// `<file>.ovt-raw.json` sidecar, the filename and size heuristics with their ranking, and the reader that
// exposes the frames as a SampleTable.

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

inline constexpr uint64_t kMaxSidecarBytes = uint64_t{1} << 20U;  // a larger sidecar is ignored
inline constexpr uint64_t kMaxRankedFrameCount = 100000;          // §8.6.6 step 4 ranking bound

// The optional `color` object of the sidecar (H.273 code points).
struct RawColor {
  bool present = false;
  uint8_t primaries = 2;  // SPEC-ASSUMPTION(#366): 2 = unspecified when the sidecar says nothing
  uint8_t transfer = 2;
  uint8_t matrix = 2;
  bool full_range = false;
};

struct RawFormat {
  uint32_t width = 0;
  uint32_t height = 0;
  PixFmt pix;
  uint32_t fps_num = 25;  // §12.1.5: the frame rate defaults to 25/1
  uint32_t fps_den = 1;
  uint64_t header_bytes = 0;  // the skipped prefix (`--raw-offset`, sidecar `header_bytes`)
  RawColor color;
  uint64_t frame_bytes = 0;  // derived: frame_bytes(pix, width, height)
};

// `WxH:pix_fmt[:fps]` with fps as `num/den` or a decimal (`29.97` is 30000/1001). nullopt for anything else.
[[nodiscard]] std::optional<RawFormat> parse_format_string(std::string_view text);

// The frame rate part alone: `num/den` or a decimal. Both parts non-zero.
[[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> parse_fps(std::string_view text);

// The sidecar (core/schema/raw_sidecar.schema.json). nullopt when `json` is not valid or lacks a required key;
// unknown keys are ignored.
[[nodiscard]] std::optional<RawFormat> parse_sidecar(std::string_view json);
[[nodiscard]] std::string sidecar_json(const RawFormat& fmt);
// "<media_path>.ovt-raw.json"; empty when `media_path` is not a file path ("fd:<n>", empty, a URI).
[[nodiscard]] std::string sidecar_path(std::string_view media_path);
// Writes the sidecar next to `media_path`; false on any failure.
[[nodiscard]] bool write_sidecar(std::string_view media_path, const RawFormat& fmt);
// Reads and parses the sidecar next to `media_path`; nullopt when absent, oversized or invalid.
[[nodiscard]] std::optional<RawFormat> read_sidecar(std::string_view media_path);

// frame_count = floor((file_size − header_bytes) / frame_bytes), 0 when the header covers the file.
[[nodiscard]] uint64_t frame_count_of(const RawFormat& fmt, uint64_t file_size) noexcept;

struct Candidate {
  RawFormat format;
  uint64_t frame_count = 0;
  bool filename_geometry = false;  // WxH came from the file name
};

// Steps 3–4 of §8.6.6: the formats compatible with the file name and size, best first. `display_name` is the
// path or name of the file (only its last path component is read).
[[nodiscard]] std::vector<Candidate> candidates(std::string_view display_name, uint64_t file_size,
                                                uint32_t fps_num = 25, uint32_t fps_den = 1, uint64_t header_bytes = 0);

// `--raw-offset` and `--raw-fps` (chapter 12 §12.1.5).
struct Overrides {
  std::optional<uint64_t> header_bytes;
  std::optional<std::pair<uint32_t, uint32_t>> fps;
};

enum class Source : uint8_t { None, Sidecar, Explicit, Heuristic };

struct Diagnostic {
  DiagCode code;
  std::string message;
  uint64_t offset = 0;
};

struct Resolution {
  Source source = Source::None;
  std::optional<RawFormat> format;    // nullopt: undetermined (status says why)
  std::vector<Candidate> candidates;  // set when the heuristics ran
  std::vector<Diagnostic> diagnostics;
  ovt_status status = OVT_OK;  // OVT_E_INVALID_ARG when format is nullopt
};

// §8.6.6 priority list: sidecar, explicit string, heuristics. The heuristics decide only when exactly one
// candidate remains; otherwise the result is CNT-RAW-0002 with the candidate list (the dialog / `ovt probe
// --raw-candidates` shows them). An unparsable explicit string is OVT_E_INVALID_ARG without falling through.
[[nodiscard]] Resolution resolve(const io::RandomAccessFile& file, std::string_view explicit_format = {},
                                 const Overrides& overrides = {});

// The reader for a known format. Never null.
[[nodiscard]] std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, const RawFormat& fmt);

}  // namespace ovt::container::raw

#endif  // OVT_CONTAINER_RAWYUV_HPP
