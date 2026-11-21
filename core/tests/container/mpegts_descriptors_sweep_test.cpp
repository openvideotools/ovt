// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Bounds and fallback behaviour of the MPEG-TS descriptor decoder (chapter 08 §8.6.7, WI-4.205): for every tag of the
// registry and for tags outside it, bodies of every length from 0 to 48 bytes with several fill patterns decode
// without reading outside the body, and every positioned field stays inside the descriptor.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "container/mpegts/descriptors.hpp"

namespace {

namespace ts = ovt::container::mpegts;
using Bytes = std::vector<uint8_t>;

Bytes pattern(size_t n, int kind) {
  Bytes b(n);
  for (size_t i = 0; i < n; ++i) {
    switch (kind) {
      case 0:
        b[i] = 0x00;
        break;
      case 1:
        b[i] = 0xFF;
        break;
      case 2:
        b[i] = static_cast<uint8_t>(i * 7U + 3U);
        break;
      case 3:
        b[i] = static_cast<uint8_t>(0x41U + i % 26U);  // letters: text fields
        break;
      default:
        b[i] = static_cast<uint8_t>(i % 2 == 0 ? 0x80U : 0x01U);
        break;
    }
  }
  return b;
}

}  // namespace

TEST_CASE("descriptor decoder: every tag and body length stays inside the body",
          "[container][mpegts][descriptors][fast]") {
  size_t decoded = 0;
  for (unsigned tag = 0; tag < 256; ++tag) {
    for (size_t len = 0; len <= 48; ++len) {
      for (int kind = 0; kind < 5; ++kind) {
        const Bytes body = pattern(len, kind);
        const std::vector<ts::DescField> fields = ts::decode_descriptor(static_cast<uint8_t>(tag), body);
        decoded += fields.size();
        for (const ts::DescField& f : fields) {
          INFO("tag " << tag << " len " << len << " kind " << kind << " field " << f.name);
          CHECK_FALSE(f.name.empty());
          if (f.bit_length != 0) {
            CHECK(uint64_t{f.bit_offset} + f.bit_length <= 16U + 8U * len);
          }
        }
      }
    }
  }
  CHECK(decoded > 0);
}

TEST_CASE("descriptor decoder: a registered tag has a name and a spec reference",
          "[container][mpegts][descriptors][fast]") {
  size_t known = 0;
  for (unsigned tag = 0; tag < 256; ++tag) {
    const ts::DescriptorInfo* info = ts::find_descriptor(static_cast<uint8_t>(tag));
    if (info == nullptr) {
      continue;
    }
    ++known;
    CHECK(info->tag == tag);
    CHECK_FALSE(info->name.empty());
    CHECK_FALSE(info->spec_ref.empty());
  }
  CHECK(known > 20);
}

TEST_CASE("descriptor decoder: hex and DVB text helpers", "[container][mpegts][descriptors][fast]") {
  CHECK(ts::to_hex(Bytes{}).empty());
  CHECK(ts::to_hex(Bytes{0x00, 0xAB, 0x0F}) == "00ab0f");
  CHECK(ts::dvb_text(Bytes{'A', 'B'}) == "AB");
  CHECK(ts::dvb_text(Bytes{0x05, 'x'}) == "x");    // character table selector dropped
  CHECK(ts::dvb_text(Bytes{0xE9}) == "\xC3\xA9");  // 0xE9 read as ISO 8859-1 é
  CHECK(ts::dvb_text(Bytes{0x8A, 'a'}) == "a");    // control code dropped
}
