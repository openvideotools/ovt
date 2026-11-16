// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.106a tests: the decoded picture hashes of H.265 D.3.19 (core/src/decode/picture_hash.cpp) over synthetic
// planes: RFC 1321 vectors for the MD5, the D.3.19 sample packing (one byte per 8-bit sample, two bytes
// little-endian above), the augmented CRC-16/CCITT, the (x, y)-masked checksum, and the SEI conversion and text.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/model/plane.hpp"
#include "ovt/cxx/syntax/h265/sei.hpp"
#include "picture_hash.hpp"

using ovt::model::PlaneView;
using ovt::model::SampleFormat;

namespace {

struct Plane {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bit_depth = 8;
  std::vector<uint8_t> bytes;  // stride = width * sample_bytes (+ 3 bytes of padding per row to test strides)

  [[nodiscard]] uint8_t sample_bytes() const { return bit_depth > 8 ? 2 : 1; }
  [[nodiscard]] uint32_t stride() const { return width * sample_bytes() + 3; }

  static Plane filled(uint32_t w, uint32_t h, uint8_t depth, uint32_t seed) {
    Plane p;
    p.width = w;
    p.height = h;
    p.bit_depth = depth;
    p.bytes.assign(static_cast<std::size_t>(p.stride()) * h, 0xEE);
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        p.set(x, y, (seed + 7 * x + 13 * y) % (1U << depth));
      }
    }
    return p;
  }
  [[nodiscard]] std::size_t at(uint32_t x, uint32_t y) const {
    return static_cast<std::size_t>(y) * stride() + static_cast<std::size_t>(x) * sample_bytes();
  }
  void set(uint32_t x, uint32_t y, uint32_t v) {
    if (sample_bytes() == 1) {
      bytes[at(x, y)] = static_cast<uint8_t>(v);
    } else {
      bytes[at(x, y)] = static_cast<uint8_t>(v & 0xFFU);
      bytes[at(x, y) + 1] = static_cast<uint8_t>(v >> 8U);
    }
  }
  [[nodiscard]] uint32_t get(uint32_t x, uint32_t y) const {
    return sample_bytes() == 1 ? bytes[at(x, y)] : static_cast<uint32_t>(bytes[at(x, y)] | (bytes[at(x, y) + 1] << 8U));
  }
  [[nodiscard]] PlaneView view(uint8_t plane) const {
    PlaneView v;
    v.data = bytes.data();
    v.width = width;
    v.height = height;
    v.stride_bytes = stride();
    v.bit_depth = bit_depth;
    v.sample_bytes = sample_bytes();
    v.format = sample_bytes() == 1 ? SampleFormat::U8 : SampleFormat::U16LE;
    v.plane = plane;
    v.stage = 0;
    return v;
  }
};

std::string hex(std::span<const uint8_t> bytes) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : bytes) {
    out.push_back(kHex[b >> 4U]);
    out.push_back(kHex[b & 0xFU]);
  }
  return out;
}

}  // namespace

// ---- D.3.19 hashes ----------------------------------------------------------------------------------------

TEST_CASE("picture_hash: MD5 matches RFC 1321 test vectors", "[fast][unit][decode][verify]") {
  using ovt::decode::Md5;
  CHECK(hex(Md5().finish()) == "d41d8cd98f00b204e9800998ecf8427e");
  {
    Md5 m;
    const std::string abc = "abc";
    m.update({reinterpret_cast<const uint8_t*>(abc.data()),
              abc.size()});  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    CHECK(hex(m.finish()) == "900150983cd24fb0d6963f7d28e17f72");
  }
  {
    // 1027 bytes (0..255 four times + "xyz") fed in uneven pieces: exercises the block buffering.
    std::vector<uint8_t> data;
    data.reserve(1027);
    for (int rep = 0; rep < 4; ++rep) {
      for (int i = 0; i < 256; ++i) {
        data.push_back(static_cast<uint8_t>(i));
      }
    }
    data.insert(data.end(), {'x', 'y', 'z'});
    Md5 m;
    std::span<const uint8_t> rest(data);
    for (const std::size_t piece : {1U, 63U, 64U, 65U, 500U}) {
      m.update(rest.first(piece));
      rest = rest.subspan(piece);
    }
    m.update(rest);
    CHECK(hex(m.finish()) == "e2301d76c2219cb28dfe5f06cb60d53d");
  }
}

TEST_CASE("picture_hash: D.3.19 MD5 packs 8-bit samples as one byte and wider samples little-endian",
          "[fast][unit][decode][verify]") {
  // 8-bit: the hash of the raw sample bytes in raster order, the stride padding excluded.
  const Plane p8 = Plane::filled(9, 2, 8, 3);
  std::vector<uint8_t> raw;
  for (uint32_t y = 0; y < 2; ++y) {
    for (uint32_t x = 0; x < 9; ++x) {
      raw.push_back(static_cast<uint8_t>(p8.get(x, y)));
    }
  }
  ovt::decode::Md5 ref;
  ref.update(raw);
  CHECK(ovt::decode::plane_md5(p8.view(0)) == ref.finish());
  std::vector<uint8_t> packed;
  ovt::decode::append_picture_data(p8.view(0), packed);
  CHECK(packed == raw);

  // 10-bit: two bytes per sample, low byte first.
  const Plane p10 = Plane::filled(5, 3, 10, 900);
  std::vector<uint8_t> raw10;
  for (uint32_t y = 0; y < 3; ++y) {
    for (uint32_t x = 0; x < 5; ++x) {
      raw10.push_back(static_cast<uint8_t>(p10.get(x, y) & 0xFFU));
      raw10.push_back(static_cast<uint8_t>(p10.get(x, y) >> 8U));
    }
  }
  ovt::decode::Md5 ref10;
  ref10.update(raw10);
  CHECK(ovt::decode::plane_md5(p10.view(0)) == ref10.finish());
}

TEST_CASE("picture_hash: D.3.19 CRC is the augmented CRC-16/CCITT (init 0xFFFF, two zero bytes appended)",
          "[fast][unit][decode][verify]") {
  // CRC-16/AUG-CCITT check value of "123456789" is 0xE5CC (the augmented form of D.3.19's algorithm).
  Plane p;
  p.width = 9;
  p.height = 1;
  p.bit_depth = 8;
  p.bytes.assign(p.stride(), 0);
  const std::string msg = "123456789";
  for (uint32_t x = 0; x < 9; ++x) {
    p.set(x, 0, static_cast<uint8_t>(msg[x]));
  }
  CHECK(ovt::decode::plane_crc(p.view(0)) == 0xE5CC);
  // Zero data of any length: the all-ones register shifted through gives a fixed, non-zero value; a one-bit change
  // changes it.
  Plane z = Plane::filled(4, 4, 8, 0);
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      z.set(x, y, 0);
    }
  }
  const uint16_t zero_crc = ovt::decode::plane_crc(z.view(0));
  z.set(3, 3, 1);
  CHECK(ovt::decode::plane_crc(z.view(0)) != zero_crc);
}

TEST_CASE("picture_hash: D.3.19 checksum sums the data bytes xor the (x, y) mask", "[fast][unit][decode][verify]") {
  Plane p = Plane::filled(2, 2, 8, 0);
  p.set(0, 0, 1);
  p.set(1, 0, 2);
  p.set(0, 1, 3);
  p.set(1, 1, 4);
  // masks: (0,0) 0, (1,0) 1, (0,1) 1, (1,1) 0 -> (1^0) + (2^1) + (3^1) + (4^0) = 1 + 3 + 2 + 4
  CHECK(ovt::decode::plane_checksum(p.view(0)) == 10);
  // 10-bit sample 0x3FF at (1,0): low byte 0xFF ^ 1 = 0xFE, high byte 0x03 ^ 1 = 0x02.
  Plane w = Plane::filled(2, 1, 10, 0);
  w.set(0, 0, 0);
  w.set(1, 0, 0x3FF);
  CHECK(ovt::decode::plane_checksum(w.view(0)) == 0xFE + 0x02);
  // x >= 256 folds its high byte into the mask: a 300-wide row of zeros sums the masks.
  Plane wide = Plane::filled(300, 1, 8, 0);
  uint32_t expect = 0;
  for (uint32_t x = 0; x < 300; ++x) {
    wide.set(x, 0, 0);
    expect += (x & 0xFFU) ^ (x >> 8U);
  }
  CHECK(ovt::decode::plane_checksum(wide.view(0)) == expect);
}

TEST_CASE("picture_hash: picture hash text and SEI conversion", "[fast][unit][decode][verify]") {
  ovt::syntax::h265::DecodedPictureHash sei;
  sei.hash_type = 1;
  sei.num_components = 3;
  sei.picture_crc = {0x1234, 0xABCD, 0x0001};
  const auto h = ovt::decode::from_sei(sei);
  CHECK(ovt::decode::to_string(h) == "crc:1234/abcd/0001");
  sei.hash_type = 2;
  sei.num_components = 1;
  sei.picture_checksum = {0xDEADBEEF, 0, 0};
  CHECK(ovt::decode::to_string(ovt::decode::from_sei(sei)) == "checksum:deadbeef");
  sei.hash_type = 0;
  sei.picture_md5[0].fill(0xAB);
  CHECK(ovt::decode::to_string(ovt::decode::from_sei(sei)) == "md5:abababababababababababababababab");
  sei.hash_type = 3;
  CHECK_FALSE(ovt::decode::kind_of(sei).has_value());
  CHECK_THROWS_AS(ovt::decode::from_sei(sei), std::invalid_argument);
  CHECK_THROWS_AS(ovt::decode::compute_picture_hash(ovt::decode::PictureHashKind::Md5, {}), std::invalid_argument);
}
