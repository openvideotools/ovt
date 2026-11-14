// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PNG goldens of the render tests (WI-1.408, §10.9.2): the downloaded Rgba8 output of a render is compared with a
// committed PNG under render/tests/golden/. The goldens are 8-bit RGBA PNGs written by write_png() below with stored
// (uncompressed) deflate blocks, so the test needs no zlib; read_png() reads exactly that subset and rejects any other
// PNG with a message. OVT_UPDATE_GOLDENS=1 rewrites the goldens from the current output instead of comparing.

#ifndef OVT_RENDER_TESTS_PNG_GOLDEN_H
#define OVT_RENDER_TESTS_PNG_GOLDEN_H

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "surface/output_download.h"

namespace ovt::render::test {

struct RgbaImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4, row-major
};

namespace png_detail {

inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

inline uint32_t adler32(const std::vector<uint8_t>& data) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t v : data) {
    a = (a + v) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16U) | a;
}

inline void put_be32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    out.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}

inline uint32_t get_be32(const std::vector<uint8_t>& in, size_t at) {
  return (uint32_t{in.at(at)} << 24U) | (uint32_t{in.at(at + 1)} << 16U) | (uint32_t{in.at(at + 2)} << 8U) |
         uint32_t{in.at(at + 3)};
}

inline void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  put_be32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  put_be32(out, crc32(body.data(), body.size()));
}

constexpr std::array<uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

}  // namespace png_detail

// 8-bit RGBA PNG, filter 0 on every row, zlib stream of stored blocks.
inline bool write_png(const std::string& path, const RgbaImage& img) {
  using namespace png_detail;
  std::vector<uint8_t> raw;
  const auto row = static_cast<size_t>(img.width) * 4;
  for (int y = 0; y < img.height; ++y) {
    raw.push_back(0);
    const auto at = static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * row);
    raw.insert(raw.end(), img.rgba.begin() + at, img.rgba.begin() + at + static_cast<std::ptrdiff_t>(row));
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + n == raw.size() ? 1 : 0);  // BFINAL, BTYPE 00
    z.push_back(static_cast<uint8_t>(n & 0xFFU));
    z.push_back(static_cast<uint8_t>(n >> 8U));
    z.push_back(static_cast<uint8_t>(~n & 0xFFU));
    z.push_back(static_cast<uint8_t>((~n >> 8U) & 0xFFU));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
             raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
  } while (pos < raw.size());
  put_be32(z, adler32(raw));

  std::vector<uint8_t> out(kSignature.begin(), kSignature.end());
  std::vector<uint8_t> ihdr;
  put_be32(ihdr, static_cast<uint32_t>(img.width));
  put_be32(ihdr, static_cast<uint32_t>(img.height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // depth 8, RGBA, deflate, filter method 0, no interlace
  put_chunk(out, "IHDR", ihdr);
  put_chunk(out, "IDAT", z);
  put_chunk(out, "IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          static_cast<std::streamsize>(out.size()));
  return f.good();
}

// Reads a PNG written by write_png(). Returns "" or the reason it cannot.
inline std::string read_png(const std::string& path, RgbaImage& img) {
  using namespace png_detail;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return "cannot open " + path;
  }
  const std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (in.size() < 8 || !std::equal(kSignature.begin(), kSignature.end(), in.begin())) {
    return path + " is not a PNG";
  }
  std::vector<uint8_t> z;
  size_t at = 8;
  while (at + 12 <= in.size()) {
    const uint32_t len = get_be32(in, at);
    const std::string type(in.begin() + static_cast<std::ptrdiff_t>(at + 4),
                           in.begin() + static_cast<std::ptrdiff_t>(at + 8));
    if (at + 12 + len > in.size()) {
      return path + ": truncated chunk " + type;  // NOLINT(performance-inefficient-string-concatenation)
    }
    const auto body = in.begin() + static_cast<std::ptrdiff_t>(at + 8);
    if (type == "IHDR") {
      const std::vector<uint8_t> h(body, body + 13);
      img.width = static_cast<int>(get_be32(h, 0));
      img.height = static_cast<int>(get_be32(h, 4));
      if (h[8] != 8 || h[9] != 6 || h[12] != 0) {
        return path + ": only 8-bit RGBA, non-interlaced goldens are supported";
      }
    } else if (type == "IDAT") {
      z.insert(z.end(), body, body + len);
    }
    at += 12 + len;
  }
  // zlib header, then stored blocks only.
  std::vector<uint8_t> raw;
  size_t p = 2;
  bool final_block = false;
  while (!final_block) {
    if (p + 5 > z.size() || (z[p] & 0x06U) != 0) {
      return path + ": IDAT is not a stored-block stream (regenerate with OVT_UPDATE_GOLDENS=1)";
    }
    final_block = (z[p] & 1U) != 0;
    const size_t n = z[p + 1] | (size_t{z[p + 2]} << 8U);
    p += 5;
    if (p + n > z.size()) {
      return path + ": truncated stored block";
    }
    raw.insert(raw.end(), z.begin() + static_cast<std::ptrdiff_t>(p), z.begin() + static_cast<std::ptrdiff_t>(p + n));
    p += n;
  }
  const auto row = static_cast<size_t>(img.width) * 4;
  if (raw.size() != (row + 1) * static_cast<size_t>(img.height)) {
    return path + ": unexpected image data size";
  }
  img.rgba.clear();
  for (int y = 0; y < img.height; ++y) {
    const size_t r = static_cast<size_t>(y) * (row + 1);
    if (raw[r] != 0) {
      return path + ": only filter type 0 is supported";
    }
    img.rgba.insert(img.rgba.end(), raw.begin() + static_cast<std::ptrdiff_t>(r + 1),
                    raw.begin() + static_cast<std::ptrdiff_t>(r + 1 + row));
  }
  return "";
}

// The Rgba8 output of a render as an image.
inline RgbaImage to_image(const ovt::render::detail::OutputPixels& px) {
  REQUIRE(px.texel_bytes == 4);
  return RgbaImage{px.width, px.height, px.bytes};
}

// Compares `actual` with render/tests/golden/<name>: same size, every channel within `tolerance` code values (GPU and
// lavapipe may round the last bit differently, §10.9.2). With OVT_UPDATE_GOLDENS=1 the golden is rewritten instead.
inline void check_golden(const std::string& name, const RgbaImage& actual, int tolerance = 1) {
  const std::string path = std::string(OVT_RENDER_GOLDEN_DIR) + "/" + name;
  const char* update = std::getenv("OVT_UPDATE_GOLDENS");  // NOLINT(concurrency-mt-unsafe): test setup
  if (update != nullptr && std::strcmp(update, "1") == 0) {
    REQUIRE(write_png(path, actual));
    WARN("golden rewritten: " << path);
    return;
  }
  RgbaImage golden;
  const std::string err = read_png(path, golden);
  INFO(err);
  REQUIRE(err.empty());
  REQUIRE(golden.width == actual.width);
  REQUIRE(golden.height == actual.height);
  size_t differing = 0;
  int worst = 0;
  for (size_t i = 0; i < golden.rgba.size(); ++i) {
    const int d = std::abs(int{golden.rgba[i]} - int{actual.rgba.at(i)});
    worst = std::max(worst, d);
    if (d > tolerance) {
      ++differing;
    }
  }
  INFO(name << ": " << differing << " channel values differ by more than " << tolerance << " (worst " << worst << ")");
  CHECK(differing == 0);
}

}  // namespace ovt::render::test

#endif  // OVT_RENDER_TESTS_PNG_GOLDEN_H
