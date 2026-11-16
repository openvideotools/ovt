// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Picture-hash conformance checks (WI-4.109); see hash_check.hpp.

#include "hash_check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

#include <fmt/format.h>

namespace ovt::analysis {

namespace {

using ovt::model::PlaneView;
using ovt::model::SampleFormat;

constexpr std::array<uint32_t, 64> kMd5K = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr std::array<uint8_t, 64> kMd5S = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                           5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                           6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

uint32_t rotl(uint32_t v, unsigned n) noexcept {
  return (v << n) | (v >> (32U - n));
}

// A plane is hashable when it is 8- or 16-bit unsigned and its rows lie inside the buffer view.
bool hashable(const PlaneView& p) noexcept {
  return p.data != nullptr && (p.format == SampleFormat::U8 || p.format == SampleFormat::U16LE) &&
         p.sample_bytes == (p.format == SampleFormat::U8 ? 1U : 2U) &&
         p.stride_bytes >= static_cast<uint64_t>(p.width) * p.sample_bytes;
}

uint32_t sample_at(const PlaneView& p, uint32_t x, uint32_t y) noexcept {
  const uint8_t* row = p.data + static_cast<std::size_t>(y) * p.stride_bytes;
  if (p.sample_bytes == 1U) {
    return row[x];
  }
  return static_cast<uint32_t>(row[2U * static_cast<std::size_t>(x)]) |
         (static_cast<uint32_t>(row[(2U * static_cast<std::size_t>(x)) + 1U]) << 8U);
}

// Calls fn(byte) for every byte of the D.3.19 picture data of the plane, in order.
template <typename Fn>
void for_each_packed_byte(const PlaneView& p, Fn&& fn) {
  const bool wide = p.bit_depth > 8U;
  for (uint32_t y = 0; y < p.height; ++y) {
    for (uint32_t x = 0; x < p.width; ++x) {
      const uint32_t v = sample_at(p, x, y);
      fn(static_cast<uint8_t>(v & 0xFFU));
      if (wide) {
        fn(static_cast<uint8_t>((v >> 8U) & 0xFFU));
      }
    }
  }
}

HashCheckResult mismatch(std::string_view plugin_id) {
  HashCheckResult r;
  r.status = HashStatus::Mismatch;
  r.message = fmt::format("decoded picture hash mismatch (plugin {})", plugin_id);
  return r;
}

HashCheckResult skipped(std::string why) {
  HashCheckResult r;
  r.status = HashStatus::Skipped;
  r.message = std::move(why);
  return r;
}

int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

Md5::Md5() noexcept : state_{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U} {}

void Md5::block(const uint8_t* p) noexcept {
  std::array<uint32_t, 16> m{};
  for (std::size_t i = 0; i < 16; ++i) {
    m[i] = static_cast<uint32_t>(p[4 * i]) | (static_cast<uint32_t>(p[(4 * i) + 1]) << 8U) |
           (static_cast<uint32_t>(p[(4 * i) + 2]) << 16U) | (static_cast<uint32_t>(p[(4 * i) + 3]) << 24U);
  }
  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  for (std::size_t i = 0; i < 64; ++i) {
    uint32_t f = 0;
    std::size_t g = 0;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = ((5 * i) + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = ((3 * i) + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const uint32_t next = b + rotl(a + f + kMd5K[i] + m[g], kMd5S[i]);
    a = d;
    d = c;
    c = b;
    b = next;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

void Md5::update(std::span<const uint8_t> data) noexcept {
  std::size_t fill = total_ % 64U;
  total_ += data.size();
  std::size_t pos = 0;
  if (fill != 0U) {
    const std::size_t take = std::min(data.size(), 64U - fill);
    std::memcpy(buffer_.data() + fill, data.data(), take);
    pos = take;
    fill += take;
    if (fill < 64U) {
      return;
    }
    block(buffer_.data());
  }
  for (; pos + 64U <= data.size(); pos += 64U) {
    block(data.data() + pos);
  }
  if (pos < data.size()) {
    std::memcpy(buffer_.data(), data.data() + pos, data.size() - pos);
  }
}

Md5Digest Md5::finish() noexcept {
  const uint64_t bits = total_ * 8U;
  const std::size_t fill = total_ % 64U;
  std::array<uint8_t, 72> pad{};
  pad[0] = 0x80U;
  const std::size_t pad_len = (fill < 56U ? 56U : 120U) - fill;
  for (std::size_t i = 0; i < 8; ++i) {
    pad[pad_len + i] = static_cast<uint8_t>((bits >> (8U * i)) & 0xFFU);
  }
  update({pad.data(), pad_len + 8U});
  Md5Digest out{};
  for (std::size_t i = 0; i < 4; ++i) {
    for (std::size_t j = 0; j < 4; ++j) {
      out[(4 * i) + j] = static_cast<uint8_t>((state_[i] >> (8U * j)) & 0xFFU);
    }
  }
  return out;
}

Md5Digest plane_md5(const PlaneView& plane) {
  Md5 md5;
  std::array<uint8_t, 4096> chunk{};
  std::size_t n = 0;
  for_each_packed_byte(plane, [&](uint8_t b) {
    chunk[n++] = b;
    if (n == chunk.size()) {
      md5.update(chunk);
      n = 0;
    }
  });
  md5.update({chunk.data(), n});
  return md5.finish();
}

uint32_t plane_crc(const PlaneView& plane) {
  // D.3.19: crc = 0xFFFF; every bit of the picture data followed by 16 zero bits is shifted in MSB first.
  uint32_t crc = 0xFFFFU;
  const auto feed = [&crc](uint8_t byte) {
    for (int bit = 7; bit >= 0; --bit) {
      const uint32_t msb = (crc >> 15U) & 1U;
      const uint32_t in = (static_cast<uint32_t>(byte) >> static_cast<unsigned>(bit)) & 1U;
      crc = (((crc << 1U) + in) & 0xFFFFU) ^ (msb * 0x1021U);
    }
  };
  for_each_packed_byte(plane, feed);
  feed(0);
  feed(0);
  return crc;
}

uint32_t plane_checksum(const PlaneView& plane) {
  uint32_t sum = 0;
  for (uint32_t y = 0; y < plane.height; ++y) {
    for (uint32_t x = 0; x < plane.width; ++x) {
      const uint32_t mask = (x & 0xFFU) ^ (y & 0xFFU) ^ (x >> 8U) ^ (y >> 8U);
      const uint32_t v = sample_at(plane, x, y);
      sum += (v & 0xFFU) ^ mask;
      if (plane.bit_depth > 8U) {
        sum += (v >> 8U) ^ mask;
      }
    }
  }
  return sum;
}

HashCheckResult check_decoded_picture_hash(const ovt::syntax::h265::DecodedPictureHash& hash,
                                           std::span<const PlaneView> planes, uint32_t pic_width, uint32_t pic_height,
                                           std::string_view plugin_id) {
  if (hash.hash_type > 2U) {
    return skipped(fmt::format("reserved hash_type {}", hash.hash_type));
  }
  const std::size_t n = hash.num_components;
  if (n == 0U || n > 3U || planes.size() < n) {
    return skipped("reconstruction planes missing");
  }
  if (!std::all_of(planes.begin(), planes.begin() + static_cast<std::ptrdiff_t>(n), hashable)) {
    return skipped("reconstruction planes are not 8/16-bit unsigned");
  }
  if (planes[0].width != pic_width || planes[0].height != pic_height) {
    return skipped("reconstruction is cropped; the hash covers the uncropped picture");
  }
  HashCheckResult result = mismatch(plugin_id);
  bool any = false;
  for (std::size_t c = 0; c < n; ++c) {
    bool differs = false;
    if (hash.hash_type == 0U) {
      differs = plane_md5(planes[c]) != hash.picture_md5[c];
    } else if (hash.hash_type == 1U) {
      differs = plane_crc(planes[c]) != hash.picture_crc[c];
    } else {
      differs = plane_checksum(planes[c]) != hash.picture_checksum[c];
    }
    result.component_mismatch[c] = differs;
    any = any || differs;
  }
  if (!any) {
    result = HashCheckResult{};
    result.status = HashStatus::Match;
  }
  return result;
}

Md5Digest frame_md5(std::span<const PlaneView> planes) {
  Md5 md5;
  std::array<uint8_t, 4096> chunk{};
  std::size_t n = 0;
  for (const PlaneView& p : planes) {
    if (!hashable(p)) {
      continue;
    }
    for_each_packed_byte(p, [&](uint8_t b) {
      chunk[n++] = b;
      if (n == chunk.size()) {
        md5.update(chunk);
        n = 0;
      }
    });
  }
  md5.update({chunk.data(), n});
  return md5.finish();
}

HashCheckResult check_frame_md5(const Md5Digest& expected, std::span<const PlaneView> planes,
                                std::string_view plugin_id) {
  if (planes.empty() ||
      !std::all_of(planes.begin(), planes.end(), [](const PlaneView& p) { return p.width == 0U || hashable(p); })) {
    return skipped("reconstruction planes missing or not 8/16-bit unsigned");
  }
  if (frame_md5(planes) == expected) {
    HashCheckResult r;
    r.status = HashStatus::Match;
    return r;
  }
  return mismatch(plugin_id);
}

std::vector<Md5Digest> parse_md5_side_file(std::string_view text) {
  std::vector<Md5Digest> out;
  while (!text.empty()) {
    const std::size_t eol = text.find('\n');
    std::string_view line = text.substr(0, eol);
    text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1U);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    if (line.size() < 32U || line.front() == '#') {
      continue;
    }
    Md5Digest d{};
    bool ok = true;
    for (std::size_t i = 0; i < 16 && ok; ++i) {
      const int hi = hex_digit(line[2 * i]);
      const int lo = hex_digit(line[(2 * i) + 1]);
      ok = hi >= 0 && lo >= 0;
      d[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (ok && (line.size() == 32U || line[32] == ' ' || line[32] == '\t' || line[32] == '\r')) {
      out.push_back(d);
    }
  }
  return out;
}

PictureHashCheck::PictureHashCheck(ovt::model::PictureStore& store, std::string plugin_id)
    : store_(store), plugin_id_(std::move(plugin_id)) {}

void PictureHashCheck::expect_h265(int decode_index, const ovt::syntax::h265::DecodedPictureHash& hash,
                                   uint32_t pic_width, uint32_t pic_height) {
  h265_[decode_index] = H265Expectation{hash, pic_width, pic_height};
}

void PictureHashCheck::expect_md5(int decode_index, const Md5Digest& digest) {
  md5_[decode_index] = digest;
}

void PictureHashCheck::check(int decode_index, const ovt::fb::FrameAnalysis& frame, std::vector<HostDiagnostic>& out) {
  const auto h = h265_.find(decode_index);
  const auto m = md5_.find(decode_index);
  if ((h == h265_.end() && m == md5_.end()) || frame.planes() == nullptr) {
    return;
  }
  std::vector<std::shared_ptr<const ovt::model::PlaneBlob>> blobs(3);
  for (const auto* ref : *frame.planes()) {
    if (ref->stage() != ovt::fb::PlaneStage::Reconstruction || ref->plane() > 2U || ref->blob_id() == nullptr) {
      continue;
    }
    blobs[ref->plane()] = store_.get(ref->blob_id()->string_view());
  }
  std::vector<PlaneView> views;
  for (const auto& blob : blobs) {
    if (blob != nullptr) {
      views.push_back(blob->view());
    }
  }
  if (views.empty()) {
    return;
  }
  HashCheckResult r;
  if (h != h265_.end()) {
    r = check_decoded_picture_hash(h->second.hash, views, h->second.width, h->second.height, plugin_id_);
  } else {
    r = check_frame_md5(m->second, views, plugin_id_);
  }
  if (r.status == HashStatus::Mismatch) {
    out.push_back({ovt::fb::Severity::Error, "VER-0004", std::move(r.message)});
  }
}

}  // namespace ovt::analysis
