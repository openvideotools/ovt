// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::container::Format and the format sniffer (chapter 08 §8.6.1, WI-1.200).

#ifndef OVT_CONTAINER_FORMAT_HPP
#define OVT_CONTAINER_FORMAT_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container {

// Names (format_name) are the ContainerTree.format strings of ovt_container.fbs.
enum class Format : uint8_t { Unknown, Isobmff, Ivf, Y4m, Ebml, Mpegts, Mxf, Hls, Dash, AnnexB, Obu, Raw };

// Which video codec an Annex B stream most likely carries, from the first NAL header (§8.6.1 step 8).
// Ambiguous: both interpretations are plausible; the stream reader decides by parsing a parameter set.
// SPEC-ASSUMPTION(#138): ovt::syntax::sniff_codec_from_nal does not exist yet, so sniff() reports Ambiguous.
enum class AnnexBCodec : uint8_t { None, H264, H265, Ambiguous };

struct SniffResult {
  Format format = Format::Unknown;
  AnnexBCodec annexb_codec = AnnexBCodec::None;  // only set for Format::AnnexB
};

[[nodiscard]] std::string_view format_name(Format f) noexcept;  // "unknown" for Format::Unknown
[[nodiscard]] std::optional<Format> format_from_name(std::string_view name) noexcept;

// Sniffs from the first 64 KiB of `file` (never more). The extension hint comes from info().display_name.
[[nodiscard]] SniffResult sniff_detail(const io::RandomAccessFile& file);
[[nodiscard]] Format sniff(const io::RandomAccessFile& file);

// The same decision on an in-memory prefix (the first min(size, 64 KiB) bytes) of a file of `file_size`
// bytes; `extension` is the lower-case file extension including the dot, or empty. Used by fuzz targets.
[[nodiscard]] SniffResult sniff_bytes(std::span<const uint8_t> prefix, uint64_t file_size, std::string_view extension);

}  // namespace ovt::container

#endif  // OVT_CONTAINER_FORMAT_HPP
