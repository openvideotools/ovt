// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal 8-bit RGB PNG writer and reader for the render goldens (chapter 10 §10.9.2 item 4). The writer stores the
// image data in uncompressed deflate blocks, so no zlib is needed; the reader accepts exactly what the writer produces
// (colour type 2, bit depth 8, no interlace, stored deflate blocks, filter type 0). A golden re-saved by another tool
// with real compression is reported as unreadable rather than misread: regenerate it with OVT_UPDATE_GOLDEN=1.
// The checked-in file stays small in git, which compresses it (the 2 MB golden cap is on the gzip size).

#ifndef OVT_RENDER_TESTS_PNG_IO_H
#define OVT_RENDER_TESTS_PNG_IO_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace ovt::render::test {

struct RgbImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgb;  // width * height * 3, rows top first
};

namespace png_detail {

inline uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
      }
      t.at(i) = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc = table.at((crc ^ data[i]) & 0xFFU) ^ (crc >> 8U);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return ~crc;
}

inline void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    out.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}

inline uint32_t get32(const std::vector<uint8_t>& in, size_t at) {
  return (uint32_t{in.at(at)} << 24U) | (uint32_t{in.at(at + 1)} << 16U) | (uint32_t{in.at(at + 2)} << 8U) |
         uint32_t{in.at(at + 3)};
}

inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  out.insert(out.end(), data.begin(), data.end());
  put32(out, crc32(&out.at(start), out.size() - start));
}

constexpr std::array<uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

}  // namespace png_detail

inline bool write_png(const std::string& path, const RgbImage& img) {
  using namespace png_detail;  // NOLINT(google-build-using-namespace)
  std::vector<uint8_t> raw;    // filter byte 0 + row
  const auto row_bytes = static_cast<size_t>(img.width) * 3;
  for (int y = 0; y < img.height; ++y) {
    raw.push_back(0);
    const auto row = img.rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * row_bytes);
    raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(row_bytes));
  }
  std::vector<uint8_t> z = {0x78, 0x01};  // zlib header: deflate, 32K window, no dictionary
  for (size_t at = 0; at < raw.size() || at == 0;) {
    const size_t n = std::min<size_t>(raw.size() - at, 65535);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);  // BFINAL, BTYPE = 00 (stored)
    z.push_back(static_cast<uint8_t>(n & 0xFFU));
    z.push_back(static_cast<uint8_t>(n >> 8U));
    z.push_back(static_cast<uint8_t>(~n & 0xFFU));
    z.push_back(static_cast<uint8_t>((~n >> 8U) & 0xFFU));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
    if (last) {
      break;
    }
  }
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t v : raw) {
    a = (a + v) % 65521U;
    b = (b + a) % 65521U;
  }
  put32(z, (b << 16U) | a);

  std::vector<uint8_t> out(kSignature.begin(), kSignature.end());
  std::vector<uint8_t> ihdr;
  put32(ihdr, static_cast<uint32_t>(img.width));
  put32(ihdr, static_cast<uint32_t>(img.height));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, RGB, deflate, filter 0, no interlace
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          static_cast<std::streamsize>(out.size()));
  return f.good();
}

inline std::optional<RgbImage> read_png(const std::string& path) {
  using namespace png_detail;  // NOLINT(google-build-using-namespace)
  std::ifstream f(path, std::ios::binary);
  const std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (in.size() < 8 || !std::equal(kSignature.begin(), kSignature.end(), in.begin())) {
    return std::nullopt;
  }
  RgbImage img;
  std::vector<uint8_t> z;
  for (size_t at = 8; at + 12 <= in.size();) {
    const uint32_t len = get32(in, at);
    const std::string type(in.begin() + static_cast<std::ptrdiff_t>(at + 4),
                           in.begin() + static_cast<std::ptrdiff_t>(at + 8));
    if (at + 12 + len > in.size()) {
      return std::nullopt;
    }
    const auto data = in.begin() + static_cast<std::ptrdiff_t>(at + 8);
    if (type == "IHDR") {
      if (len != 13 || data[8] != 8 || data[9] != 2 || data[12] != 0) {
        return std::nullopt;
      }
      img.width = static_cast<int>(get32(in, at + 8));
      img.height = static_cast<int>(get32(in, at + 12));
    } else if (type == "IDAT") {
      z.insert(z.end(), data, data + len);
    }
    at += 12 + size_t{len};
  }
  const auto row_bytes = static_cast<size_t>(img.width) * 3;
  std::vector<uint8_t> raw;
  // Stored deflate blocks only (see the file comment).
  for (size_t at = 2; at + 5 <= z.size();) {
    const uint8_t hdr = z.at(at);
    if ((hdr & 0x6U) != 0) {
      return std::nullopt;  // compressed block
    }
    const size_t n = size_t{z.at(at + 1)} | (size_t{z.at(at + 2)} << 8U);
    if (at + 5 + n > z.size()) {
      return std::nullopt;
    }
    raw.insert(raw.end(), z.begin() + static_cast<std::ptrdiff_t>(at + 5),
               z.begin() + static_cast<std::ptrdiff_t>(at + 5 + n));
    at += 5 + n;
    if ((hdr & 1U) != 0) {
      break;
    }
  }
  if (img.width <= 0 || img.height <= 0 || raw.size() != (row_bytes + 1) * static_cast<size_t>(img.height)) {
    return std::nullopt;
  }
  for (int y = 0; y < img.height; ++y) {
    const auto row = raw.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * (row_bytes + 1));
    if (*row != 0) {
      return std::nullopt;  // only filter type 0
    }
    img.rgb.insert(img.rgb.end(), row + 1, row + 1 + static_cast<std::ptrdiff_t>(row_bytes));
  }
  return img;
}

}  // namespace ovt::render::test

#endif  // OVT_RENDER_TESTS_PNG_IO_H
