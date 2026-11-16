// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Decoded picture hashes per H.265 §D.3.19 (WI-4.106 step 3). See picture_hash.hpp.

#include "picture_hash.hpp"

#include <algorithm>
#include <bit>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ovt::decode {

namespace {

// ---- RFC 1321 ---------------------------------------------------------------------------------------------

constexpr std::array<std::uint32_t, 64> kMd5K = {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
    0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
    0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
    0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
    0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
    0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
    0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
    0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};

constexpr std::array<std::uint32_t, 64> kMd5S = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                                 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                                 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                                 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

std::uint32_t load_le32(std::span<const std::uint8_t> p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

void store_le32(std::uint32_t v, std::span<std::uint8_t> p) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8U);
  p[2] = static_cast<std::uint8_t>(v >> 16U);
  p[3] = static_cast<std::uint8_t>(v >> 24U);
}

// ---- D.3.19 sample access ---------------------------------------------------------------------------------

// The plane memory as a span (§6.6.1: stride * (height - 1) + width * sample_bytes bytes).
std::span<const std::uint8_t> plane_bytes(const ovt::model::PlaneView& plane) noexcept {
  const std::size_t size = static_cast<std::size_t>(plane.stride_bytes) * (plane.height - 1) +
                           static_cast<std::size_t>(plane.width) * plane.sample_bytes;
  return {plane.data, size};
}

// The sample at (x, y) of `plane`, as the integer the decoder produced (the low 16 bits of a 4-byte sample).
std::uint32_t sample_at(const ovt::model::PlaneView& plane, std::span<const std::uint8_t> bytes, std::uint32_t x,
                        std::uint32_t y) noexcept {
  const std::size_t at =
      static_cast<std::size_t>(y) * plane.stride_bytes + static_cast<std::size_t>(x) * plane.sample_bytes;
  switch (plane.sample_bytes) {
    case 1:
      return bytes[at];
    case 2:
      return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8U);
    default:
      return load_le32(bytes.subspan(at, 4)) & 0xFFFFU;
  }
}

void check_plane(const ovt::model::PlaneView& plane) {
  if (plane.width == 0 || plane.height == 0) {
    throw std::invalid_argument("picture_hash: empty plane");
  }
  if (plane.data == nullptr) {
    throw std::invalid_argument("picture_hash: plane without data");
  }
  if (plane.sample_bytes != 1 && plane.sample_bytes != 2 && plane.sample_bytes != 4) {
    throw std::invalid_argument("picture_hash: sample_bytes must be 1, 2 or 4");
  }
  if (static_cast<std::uint64_t>(plane.width) * plane.sample_bytes > plane.stride_bytes) {
    throw std::invalid_argument("picture_hash: stride_bytes smaller than a row");
  }
}

// Calls f(byte) for every byte of the D.3.19 pictureData sequence of `plane`, in order.
template <class F>
void for_each_picture_byte(const ovt::model::PlaneView& plane, F&& f) {
  const bool wide = plane.bit_depth > 8;
  const std::span<const std::uint8_t> bytes = plane_bytes(plane);
  for (std::uint32_t y = 0; y < plane.height; ++y) {
    for (std::uint32_t x = 0; x < plane.width; ++x) {
      const std::uint32_t s = sample_at(plane, bytes, x, y);
      f(static_cast<std::uint8_t>(s & 0xFFU), x, y);
      if (wide) {
        f(static_cast<std::uint8_t>((s >> 8U) & 0xFFU), x, y);
      }
    }
  }
}

constexpr std::string_view kHex = "0123456789abcdef";

void append_hex(std::string& out, std::uint64_t value, int digits) {
  for (int i = digits - 1; i >= 0; --i) {
    out.push_back(kHex[(value >> (4 * static_cast<unsigned>(i))) & 0xFU]);
  }
}

}  // namespace

// ---- Md5 --------------------------------------------------------------------------------------------------

Md5::Md5() noexcept : state_{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U} {}

void Md5::block(std::span<const std::uint8_t, 64> chunk) noexcept {
  std::array<std::uint32_t, 16> m{};
  for (std::size_t i = 0; i < 16; ++i) {
    m[i] = load_le32(chunk.subspan(4 * i, 4));
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  for (std::uint32_t i = 0; i < 64; ++i) {
    std::uint32_t f = 0;
    std::uint32_t g = 0;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const std::uint32_t tmp = d;
    d = c;
    c = b;
    b = b + std::rotl(a + f + kMd5K[i] + m[g], static_cast<int>(kMd5S[i]));
    a = tmp;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

void Md5::update(std::span<const std::uint8_t> bytes) noexcept {
  total_ += bytes.size();
  std::size_t pos = 0;
  if (buffered_ > 0) {
    const std::size_t take = std::min(bytes.size(), buffer_.size() - buffered_);
    std::copy_n(bytes.begin(), take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
    buffered_ += take;
    pos = take;
    if (buffered_ < buffer_.size()) {
      return;
    }
    block(std::span<const std::uint8_t, 64>(buffer_));
    buffered_ = 0;
  }
  for (; pos + 64 <= bytes.size(); pos += 64) {
    block(bytes.subspan(pos).first<64>());
  }
  if (pos < bytes.size()) {
    buffered_ = bytes.size() - pos;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(pos), buffered_, buffer_.begin());
  }
}

std::array<std::uint8_t, 16> Md5::finish() noexcept {
  const std::uint64_t bit_length = total_ * 8U;
  const std::uint8_t one = 0x80U;
  update({&one, 1});
  const std::uint8_t zero = 0U;
  while (buffered_ != 56) {
    update({&zero, 1});
  }
  std::array<std::uint8_t, 8> len{};
  for (std::size_t i = 0; i < 8; ++i) {
    len[i] = static_cast<std::uint8_t>(bit_length >> (8 * i));
  }
  update(len);
  std::array<std::uint8_t, 16> digest{};
  for (std::size_t i = 0; i < 4; ++i) {
    store_le32(state_[i], std::span<std::uint8_t>(digest).subspan(4 * i, 4));
  }
  return digest;
}

// ---- D.3.19 -----------------------------------------------------------------------------------------------

void append_picture_data(const ovt::model::PlaneView& plane, std::vector<std::uint8_t>& out) {
  check_plane(plane);
  const std::size_t per_sample = plane.bit_depth > 8 ? 2 : 1;
  out.reserve(out.size() + static_cast<std::size_t>(plane.width) * plane.height * per_sample);
  for_each_picture_byte(plane, [&out](std::uint8_t b, std::uint32_t /*x*/, std::uint32_t /*y*/) { out.push_back(b); });
}

std::array<std::uint8_t, 16> plane_md5(const ovt::model::PlaneView& plane) {
  check_plane(plane);
  Md5 md5;
  // Row by row through the pictureData packing; a wide plane doubles its byte count.
  const bool wide = plane.bit_depth > 8;
  const std::span<const std::uint8_t> bytes = plane_bytes(plane);
  std::vector<std::uint8_t> row_bytes;
  row_bytes.reserve(static_cast<std::size_t>(plane.width) * (wide ? 2 : 1));
  for (std::uint32_t y = 0; y < plane.height; ++y) {
    row_bytes.clear();
    for (std::uint32_t x = 0; x < plane.width; ++x) {
      const std::uint32_t s = sample_at(plane, bytes, x, y);
      row_bytes.push_back(static_cast<std::uint8_t>(s & 0xFFU));
      if (wide) {
        row_bytes.push_back(static_cast<std::uint8_t>((s >> 8U) & 0xFFU));
      }
    }
    md5.update(row_bytes);
  }
  return md5.finish();
}

std::uint16_t plane_crc(const ovt::model::PlaneView& plane) {
  check_plane(plane);
  std::uint32_t crc = 0xFFFFU;
  const auto feed = [&crc](std::uint8_t byte) {
    for (int bit_idx = 0; bit_idx < 8; ++bit_idx) {
      const std::uint32_t crc_msb = (crc >> 15U) & 1U;
      const std::uint32_t bit_val = (static_cast<std::uint32_t>(byte) >> static_cast<unsigned>(7 - bit_idx)) & 1U;
      crc = (((crc << 1U) + bit_val) & 0xFFFFU) ^ (crc_msb * 0x1021U);
    }
  };
  for_each_picture_byte(plane, [&feed](std::uint8_t b, std::uint32_t /*x*/, std::uint32_t /*y*/) { feed(b); });
  feed(0);  // D.3.19: pictureData[dataLen] = 0 and pictureData[dataLen + 1] = 0
  feed(0);
  return static_cast<std::uint16_t>(crc);
}

std::uint32_t plane_checksum(const ovt::model::PlaneView& plane) {
  check_plane(plane);
  std::uint32_t sum = 0;
  for_each_picture_byte(plane, [&sum](std::uint8_t b, std::uint32_t x, std::uint32_t y) {
    const std::uint32_t xor_mask = (x & 0xFFU) ^ (y & 0xFFU) ^ (x >> 8U) ^ (y >> 8U);
    sum += static_cast<std::uint32_t>(b) ^ xor_mask;  // D.3.19: no masking of the sum term
  });
  return sum;
}

PictureHash compute_picture_hash(PictureHashKind kind, std::span<const ovt::model::PlaneView> planes) {
  if (planes.empty() || planes.size() > 3) {
    throw std::invalid_argument("compute_picture_hash: 1 to 3 planes expected");
  }
  PictureHash out;
  out.kind = kind;
  out.num_components = static_cast<std::uint32_t>(planes.size());
  for (std::size_t c = 0; c < planes.size(); ++c) {
    switch (kind) {
      case PictureHashKind::Md5:
        out.components[c].md5 = plane_md5(planes[c]);
        break;
      case PictureHashKind::Crc:
        out.components[c].crc = plane_crc(planes[c]);
        break;
      case PictureHashKind::Checksum:
        out.components[c].checksum = plane_checksum(planes[c]);
        break;
    }
  }
  return out;
}

std::optional<PictureHashKind> kind_of(const ovt::syntax::h265::DecodedPictureHash& sei) noexcept {
  switch (sei.hash_type) {
    case 0:
      return PictureHashKind::Md5;
    case 1:
      return PictureHashKind::Crc;
    case 2:
      return PictureHashKind::Checksum;
    default:
      return std::nullopt;
  }
}

PictureHash from_sei(const ovt::syntax::h265::DecodedPictureHash& sei) {
  const auto kind = kind_of(sei);
  if (!kind.has_value()) {
    throw std::invalid_argument("from_sei: reserved decoded_picture_hash hash_type");
  }
  PictureHash out;
  out.kind = *kind;
  out.num_components = std::min<std::uint32_t>(sei.num_components, 3);
  for (std::uint32_t c = 0; c < out.num_components; ++c) {
    out.components[c].md5 = sei.picture_md5[c];
    out.components[c].crc = static_cast<std::uint16_t>(sei.picture_crc[c]);
    out.components[c].checksum = sei.picture_checksum[c];
  }
  return out;
}

bool operator==(const PictureHash& a, const PictureHash& b) noexcept {
  if (a.kind != b.kind || a.num_components != b.num_components || a.num_components > 3) {
    return false;
  }
  for (std::uint32_t c = 0; c < a.num_components; ++c) {
    switch (a.kind) {
      case PictureHashKind::Md5:
        if (a.components[c].md5 != b.components[c].md5) {
          return false;
        }
        break;
      case PictureHashKind::Crc:
        if (a.components[c].crc != b.components[c].crc) {
          return false;
        }
        break;
      case PictureHashKind::Checksum:
        if (a.components[c].checksum != b.components[c].checksum) {
          return false;
        }
        break;
    }
  }
  return true;
}

std::string to_string(const PictureHash& hash) {
  std::string out;
  switch (hash.kind) {
    case PictureHashKind::Md5:
      out = "md5:";
      break;
    case PictureHashKind::Crc:
      out = "crc:";
      break;
    case PictureHashKind::Checksum:
      out = "checksum:";
      break;
  }
  const std::uint32_t n = std::min<std::uint32_t>(hash.num_components, 3);
  for (std::uint32_t c = 0; c < n; ++c) {
    if (c > 0) {
      out.push_back('/');
    }
    switch (hash.kind) {
      case PictureHashKind::Md5:
        for (const std::uint8_t byte : hash.components[c].md5) {
          append_hex(out, byte, 2);
        }
        break;
      case PictureHashKind::Crc:
        append_hex(out, hash.components[c].crc, 4);
        break;
      case PictureHashKind::Checksum:
        append_hex(out, hash.components[c].checksum, 8);
        break;
    }
  }
  return out;
}

}  // namespace ovt::decode
