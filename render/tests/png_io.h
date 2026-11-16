// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal 8-bit RGB PNG writer and reader for the render goldens (§10.9.2 item 4), without zlib: stored deflate blocks,
// or with `compress` one fixed-Huffman block matching the previous pixel or the pixel above (WI-2.432: a stored 1080p
// golden is 6 MB, over the 2 MB file cap). The reader takes exactly that (RGB8, no interlace, filter 0); a golden
// re-saved with dynamic Huffman blocks is reported unreadable rather than misread: regenerate it (OVT_UPDATE_GOLDEN=1).

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

// RFC 1951 §3.2.5 length codes 257..285 and distance codes 0..29: base values; extra bits by formula.
constexpr int len_extra(size_t lc) {
  return (lc < 8 || lc == 28) ? 0 : static_cast<int>((lc - 4) / 4);
}
constexpr int dist_extra(size_t dc) {
  return dc < 4 ? 0 : static_cast<int>((dc - 2) / 2);
}
constexpr std::array<uint16_t, 29> kLenBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint16_t, 30> kDistBase = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};

class BitWriter {
 public:
  explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
  void bits(uint32_t v, int n) {  // LSB first
    for (int i = 0; i < n; ++i) {
      push((v >> static_cast<unsigned>(i)) & 1U);
    }
  }
  void code(uint32_t v, int n) {  // Huffman codes go MSB first
    for (int i = n - 1; i >= 0; --i) {
      push((v >> static_cast<unsigned>(i)) & 1U);
    }
  }
  void flush() {
    if (used_ > 0) {
      out_.push_back(cur_);
      cur_ = 0;
      used_ = 0;
    }
  }

 private:
  void push(uint32_t b) {
    cur_ = static_cast<uint8_t>(cur_ | (b << static_cast<unsigned>(used_)));
    if (++used_ == 8) {
      flush();
    }
  }
  std::vector<uint8_t>& out_;
  uint8_t cur_ = 0;
  int used_ = 0;
};

inline void fixed_symbol(BitWriter& w, uint32_t sym) {
  if (sym < 144) {
    w.code(0x30 + sym, 8);
  } else if (sym < 256) {
    w.code(0x190 + sym - 144, 9);
  } else if (sym < 280) {
    w.code(sym - 256, 7);
  } else {
    w.code(0xC0 + sym - 280, 8);
  }
}

// One final fixed-Huffman block. Matches are tried at distance 3 (previous pixel) and `stride` (the pixel above).
inline void deflate_fixed(const std::vector<uint8_t>& raw, size_t stride, std::vector<uint8_t>& z) {
  BitWriter w(z);
  w.bits(3, 3);  // BFINAL = 1, BTYPE = 01 (fixed Huffman)
  size_t i = 0;
  while (i < raw.size()) {
    size_t best = 0;
    size_t dist = 0;
    for (const size_t d : {size_t{3}, stride}) {
      if (d == 0 || d > i || d > 32768) {
        continue;
      }
      size_t n = 0;
      while (n < 258 && i + n < raw.size() && raw[i + n] == raw[i + n - d]) {
        ++n;
      }
      if (n > best) {
        best = n;
        dist = d;
      }
    }
    if (best < 3) {
      fixed_symbol(w, raw[i]);
      ++i;
      continue;
    }
    size_t lc = kLenBase.size() - 1;
    while (kLenBase.at(lc) > best) {
      --lc;
    }
    fixed_symbol(w, static_cast<uint32_t>(257 + lc));
    w.bits(static_cast<uint32_t>(best - kLenBase.at(lc)), len_extra(lc));
    size_t dc = kDistBase.size() - 1;
    while (kDistBase.at(dc) > dist) {
      --dc;
    }
    w.code(static_cast<uint32_t>(dc), 5);
    w.bits(static_cast<uint32_t>(dist - kDistBase.at(dc)), dist_extra(dc));
    i += best;
  }
  fixed_symbol(w, 256);
  w.flush();
}

class BitReader {
 public:
  BitReader(const std::vector<uint8_t>& in, size_t at) : in_(in), pos_(at * 8) {}
  bool bits(int n, uint32_t& v) {
    v = 0;
    for (int i = 0; i < n; ++i) {
      uint32_t b = 0;
      if (!bit(b)) {
        return false;
      }
      v |= b << static_cast<unsigned>(i);
    }
    return true;
  }
  bool bit(uint32_t& b) {
    if (pos_ / 8 >= in_.size()) {
      return false;
    }
    b = (in_[pos_ / 8] >> (pos_ % 8)) & 1U;
    ++pos_;
    return true;
  }
  void align() { pos_ = (pos_ + 7) / 8 * 8; }
  void seek(size_t byte) { pos_ = byte * 8; }
  [[nodiscard]] size_t byte_pos() const { return pos_ / 8; }

 private:
  const std::vector<uint8_t>& in_;
  size_t pos_;
};

// Decodes one fixed-Huffman literal/length symbol (RFC 1951 §3.2.6); -1 on malformed input.
inline int read_fixed_symbol(BitReader& r) {
  uint32_t code = 0;
  for (int len = 1; len <= 9; ++len) {
    uint32_t b = 0;
    if (!r.bit(b)) {
      return -1;
    }
    code = (code << 1U) | b;
    if (len == 7 && code <= 0x17) {
      return static_cast<int>(256 + code);
    }
    if (len == 8 && code >= 0x30 && code <= 0xBF) {
      return static_cast<int>(code - 0x30);
    }
    if (len == 8 && code >= 0xC0 && code <= 0xC7) {
      return static_cast<int>(280 + code - 0xC0);
    }
    if (len == 9 && code >= 0x190) {
      return static_cast<int>(144 + code - 0x190);
    }
  }
  return -1;
}

inline bool inflate_fixed(BitReader& r, std::vector<uint8_t>& out) {
  for (;;) {
    const int sym = read_fixed_symbol(r);
    if (sym < 0 || sym > 285) {
      return false;
    }
    if (sym < 256) {
      out.push_back(static_cast<uint8_t>(sym));
      continue;
    }
    if (sym == 256) {
      return true;
    }
    const auto lc = static_cast<size_t>(sym - 257);
    uint32_t extra = 0;
    if (lc >= kLenBase.size() || !r.bits(len_extra(lc), extra)) {
      return false;
    }
    const size_t len = kLenBase.at(lc) + extra;
    uint32_t dcode = 0;
    for (int i = 0; i < 5; ++i) {
      uint32_t b = 0;
      if (!r.bit(b)) {
        return false;
      }
      dcode = (dcode << 1U) | b;
    }
    if (dcode >= kDistBase.size() || !r.bits(dist_extra(dcode), extra)) {
      return false;
    }
    const size_t dist = kDistBase.at(dcode) + extra;
    if (dist > out.size()) {
      return false;
    }
    for (size_t k = 0; k < len; ++k) {
      out.push_back(out[out.size() - dist]);
    }
  }
}

// Stored or fixed-Huffman deflate blocks after the 2-byte zlib header (see the file comment).
inline bool inflate(const std::vector<uint8_t>& z, std::vector<uint8_t>& raw) {
  BitReader r(z, 2);
  for (;;) {
    uint32_t final_block = 0;
    uint32_t type = 0;
    if (!r.bits(1, final_block) || !r.bits(2, type)) {
      return false;
    }
    if (type == 1) {
      if (!inflate_fixed(r, raw)) {
        return false;
      }
    } else if (type == 0) {
      r.align();
      const size_t at = r.byte_pos();
      if (at + 4 > z.size()) {
        return false;
      }
      const size_t n = size_t{z.at(at)} | (size_t{z.at(at + 1)} << 8U);
      if (at + 4 + n > z.size()) {
        return false;
      }
      raw.insert(raw.end(), z.begin() + static_cast<std::ptrdiff_t>(at + 4),
                 z.begin() + static_cast<std::ptrdiff_t>(at + 4 + n));
      r.seek(at + 4 + n);
    } else {
      return false;  // dynamic Huffman
    }
    if (final_block != 0) {
      return true;
    }
  }
}

}  // namespace png_detail

inline bool write_png(const std::string& path, const RgbImage& img, bool compress = false) {
  using namespace png_detail;  // NOLINT(google-build-using-namespace)
  std::vector<uint8_t> raw;    // filter byte 0 + row
  const auto row_bytes = static_cast<size_t>(img.width) * 3;
  for (int y = 0; y < img.height; ++y) {
    raw.push_back(0);
    const auto row = img.rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * row_bytes);
    raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(row_bytes));
  }
  std::vector<uint8_t> z = {0x78, 0x01};  // zlib header: deflate, 32K window, no dictionary
  for (size_t at = 0; !compress && (at < raw.size() || at == 0);) {
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
  if (compress) {
    deflate_fixed(raw, row_bytes + 1, z);
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
  if (!inflate(z, raw)) {
    return std::nullopt;
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
