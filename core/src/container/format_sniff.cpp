// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Format sniffing (chapter 08 §8.6.1, WI-1.200): first match wins, never more than 64 KiB read.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "container/format.hpp"
#include "container/obu.hpp"

namespace ovt::container {
namespace {

constexpr size_t kWindow = size_t{64} * 1024;
constexpr size_t kDashWindow = 4096;
constexpr size_t kMaxLeadingZeros = 64;

bool starts_with(std::span<const uint8_t> d, std::string_view s) {
  return d.size() >= s.size() &&
         std::equal(s.begin(), s.end(), d.begin(), [](char c, uint8_t b) { return static_cast<uint8_t>(c) == b; });
}

bool has_at(std::span<const uint8_t> d, size_t pos, std::string_view s) {
  return pos <= d.size() && starts_with(d.subspan(pos), s);
}

uint32_t be32(std::span<const uint8_t> d, size_t pos) {
  return (static_cast<uint32_t>(d[pos]) << 24U) | (static_cast<uint32_t>(d[pos + 1]) << 16U) |
         (static_cast<uint32_t>(d[pos + 2]) << 8U) | static_cast<uint32_t>(d[pos + 3]);
}

// Step 3.
bool is_isobmff(std::span<const uint8_t> d) {
  static constexpr std::array<std::string_view, 13> kTypes = {"ftyp", "styp", "moov", "moof", "mdat", "free", "skip",
                                                              "wide", "uuid", "sidx", "pdin", "emsg", "prft"};
  if (d.size() < 8) {
    return false;
  }
  const bool type_ok = std::any_of(kTypes.begin(), kTypes.end(), [&](std::string_view t) { return has_at(d, 4, t); });
  if (!type_ok) {
    return false;
  }
  const uint32_t size = be32(d, 0);
  return size >= 8 || size == 0 || size == 1;
}

// Step 5: sync bytes at three packet strides (188 plain TS, 192 M2TS, 204 with FEC).
bool is_mpegts(std::span<const uint8_t> d) {
  static constexpr std::array<size_t, 3> kStrides = {188, 192, 204};
  return std::ranges::any_of(kStrides, [&](size_t stride) {
    return d.size() > 2 * stride && d[0] == 0x47 && d[stride] == 0x47 && d[2 * stride] == 0x47;
  });
}

// Step 6: the KLV key start at offset 0, or a partition pack key within the run-in (up to 64 KiB).
bool is_mxf(std::span<const uint8_t> d) {
  static constexpr std::array<uint8_t, 4> kKey = {0x06, 0x0E, 0x2B, 0x34};
  static constexpr std::array<uint8_t, 11> kPartitionPack = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05,
                                                             0x01, 0x01, 0x0D, 0x01, 0x02};
  if (d.size() >= kKey.size() && std::equal(kKey.begin(), kKey.end(), d.begin())) {
    return true;
  }
  return std::search(d.begin(), d.end(), kPartitionPack.begin(), kPartitionPack.end()) != d.end();
}

// Step 7.
bool contains_mpd(std::span<const uint8_t> d) {
  const auto head = d.first(std::min(d.size(), kDashWindow));
  static constexpr std::string_view kMpd = "<MPD";
  return std::search(head.begin(), head.end(), kMpd.begin(), kMpd.end(),
                     [](uint8_t b, char c) { return b == static_cast<uint8_t>(c); }) != head.end();
}

// Step 8: returns the offset of the first NAL header byte, or 0 when there is no Annex B start code.
size_t annexb_nal_offset(std::span<const uint8_t> d) {
  size_t zeros = 0;
  while (zeros < d.size() && d[zeros] == 0 && zeros <= kMaxLeadingZeros + 2) {
    ++zeros;
  }
  if (zeros >= 2 && zeros <= kMaxLeadingZeros + 2 && zeros < d.size() && d[zeros] == 1) {
    return zeros + 1;
  }
  return 0;
}

AnnexBCodec guess_annexb_codec(std::span<const uint8_t> d, size_t nal) {
  if (nal >= d.size()) {
    return AnnexBCodec::None;
  }
  const uint8_t b0 = d[nal];
  if ((b0 & 0x80U) != 0) {
    return AnnexBCodec::None;
  }
  const unsigned h264_type = b0 & 0x1FU;
  const bool h264 = h264_type == 7 || h264_type == 9 || h264_type == 6 || h264_type == 5 || h264_type == 1;
  const unsigned h265_type = (b0 >> 1U) & 0x3FU;
  const bool h265_type_ok = h265_type == 32 || h265_type == 33 || h265_type == 34 || h265_type == 35 ||
                            h265_type == 39 || h265_type == 19 || h265_type == 20 || h265_type == 21;
  const bool h265 = h265_type_ok && nal + 1 < d.size() && (d[nal + 1] & 0x07U) != 0;
  if (h264 && h265) {
    return AnnexBCodec::Ambiguous;
  }
  if (h265) {
    return AnnexBCodec::H265;
  }
  return h264 ? AnnexBCodec::H264 : AnnexBCodec::None;
}

bool is_obu(std::span<const uint8_t> d, uint64_t file_size) {
  return obu_detect(d, file_size).framing != ObuFraming::NotAv1;
}

bool raw_extension(std::string_view ext) {
  return ext == ".yuv" || ext == ".raw" || ext == ".rgb";
}

std::string extension_of(std::string_view display_name) {
  const auto slash = display_name.find_last_of("/\\");
  const auto base = slash == std::string_view::npos ? display_name : display_name.substr(slash + 1);
  const auto dot = base.find_last_of('.');
  if (dot == std::string_view::npos) {
    return {};
  }
  std::string ext(base.substr(dot));
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

}  // namespace

std::string_view format_name(Format f) noexcept {
  switch (f) {
    case Format::Isobmff:
      return "isobmff";
    case Format::Ivf:
      return "ivf";
    case Format::Y4m:
      return "y4m";
    case Format::Ebml:
      return "ebml";
    case Format::Mpegts:
      return "mpegts";
    case Format::Mxf:
      return "mxf";
    case Format::Hls:
      return "hls";
    case Format::Dash:
      return "dash";
    case Format::AnnexB:
      return "annexb";
    case Format::Obu:
      return "obu";
    case Format::Raw:
      return "raw";
    case Format::Unknown:
      break;
  }
  return "unknown";
}

std::optional<Format> format_from_name(std::string_view name) noexcept {
  for (const Format f : {Format::Isobmff, Format::Ivf, Format::Y4m, Format::Ebml, Format::Mpegts, Format::Mxf,
                         Format::Hls, Format::Dash, Format::AnnexB, Format::Obu, Format::Raw}) {
    if (format_name(f) == name) {
      return f;
    }
  }
  return std::nullopt;
}

SniffResult sniff_bytes(std::span<const uint8_t> prefix, uint64_t file_size, std::string_view extension) {
  const auto d = prefix.first(std::min(prefix.size(), kWindow));
  if (starts_with(d, "DKIF")) {
    return {Format::Ivf, AnnexBCodec::None};
  }
  if (starts_with(d, "YUV4MPEG2 ")) {
    return {Format::Y4m, AnnexBCodec::None};
  }
  if (is_isobmff(d)) {
    return {Format::Isobmff, AnnexBCodec::None};
  }
  if (d.size() >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3) {
    return {Format::Ebml, AnnexBCodec::None};
  }
  if (is_mpegts(d)) {
    return {Format::Mpegts, AnnexBCodec::None};
  }
  if (is_mxf(d)) {
    return {Format::Mxf, AnnexBCodec::None};
  }
  if (starts_with(d, "#EXTM3U")) {
    return {Format::Hls, AnnexBCodec::None};
  }
  if ((starts_with(d, "<?xml") || starts_with(d, "<MPD")) && contains_mpd(d)) {
    return {Format::Dash, AnnexBCodec::None};
  }
  if (const size_t nal = annexb_nal_offset(d); nal != 0) {
    return {Format::AnnexB, guess_annexb_codec(d, nal)};
  }
  if (is_obu(d, file_size)) {
    return {Format::Obu, AnnexBCodec::None};
  }
  if (raw_extension(extension)) {
    return {Format::Raw, AnnexBCodec::None};
  }
  return {};
}

SniffResult sniff_detail(const io::RandomAccessFile& file) {
  // On the heap: job threads on macOS and Android can have small stacks.
  std::vector<uint8_t> buf(static_cast<size_t>(std::min<uint64_t>(file.size(), kWindow)));
  const size_t n = buf.empty() ? 0 : file.read(0, buf.size(), buf.data());
  return sniff_bytes(std::span<const uint8_t>(buf.data(), n), file.size(), extension_of(file.info().display_name));
}

Format sniff(const io::RandomAccessFile& file) {
  return sniff_detail(file).format;
}

}  // namespace ovt::container
