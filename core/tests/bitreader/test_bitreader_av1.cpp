// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of ovt::BitReader, AV1 rows of chapter 7 §7.2.7 (WI-1.103): leb128, uvlc, su, ns, le, delta_q,
// trailing_bits, byte_alignment and the L(n) guard. Vectors are from §7.2.7 and the WI-1.103 acceptance tests.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"

namespace {

using ovt::BitReader;
using ovt::ParseError;
using Bytes = std::vector<uint8_t>;

// "0101 1" → bytes, MSB first, zero-padded to a whole byte. Spaces are ignored.
Bytes from_bits(std::string_view bits) {
  Bytes out;
  unsigned n = 0;
  for (const char c : bits) {
    if (c == ' ') {
      continue;
    }
    if (n % 8 == 0) {
      out.push_back(0);
    }
    if (c == '1') {
      out.back() = static_cast<uint8_t>(out.back() | (0x80U >> (n % 8)));
    }
    ++n;
  }
  return out;
}

// Runs fn, which must throw ParseError with `code`; returns the error.
ParseError expect_error(const std::function<void()>& fn, const std::string& code) {
  try {
    fn();
  } catch (const ParseError& e) {
    CHECK(e.code == code);
    return e;
  }
  FAIL("no ParseError " << code << " thrown");
  return ParseError("", "", 0);
}

struct Warning {
  std::string code;
  std::string message;
  uint64_t bit_offset;
};

// Stands in for SyntaxBuilder (WI-1.106), which attaches itself as the reader's sink (§7.3.2).
struct RecordingSink final : BitReader::DiagnosticSink {
  std::vector<Warning> warnings;
  void reader_warning(std::string_view code, std::string_view message, uint64_t bit_offset) override {
    warnings.push_back(Warning{std::string(code), std::string(message), bit_offset});
  }
};

}  // namespace

TEST_CASE("br_leb128", "[bitreader][av1][fast]") {
  struct Case {
    Bytes in;
    uint64_t value;
    uint64_t bits;
  };
  const std::vector<Case> cases = {
      {{0x00}, 0, 8},
      {{0x7F}, 127, 8},
      {{0x80, 0x01}, 128, 16},
      {{0xE5, 0x8E, 0x26}, 624485, 24},
      {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}, (uint64_t{1} << 56) - 1, 64},  // acceptance 1
      {{0x05, 0xAA}, 5, 8},  // stops at the first byte without continuation
  };
  for (const auto& c : cases) {
    RecordingSink sink;
    BitReader br(c.in);
    br.set_diagnostic_sink(&sink);
    CHECK(br.leb128() == c.value);
    CHECK(br.bit_pos() == c.bits);
    CHECK(sink.warnings.empty());
  }

  SECTION("unaligned start") {
    const Bytes b = from_bits("1 10000000 00000001");  // one bit, then 80 01
    BitReader br(b);
    (void)br.u(1);
    CHECK(br.leb128() == 128);
    CHECK(br.bit_pos() == 17);
  }

  SECTION("truncated: BR-0001 and position unchanged") {
    const Bytes b = {0x00, 0x80, 0x80};
    BitReader br(b);
    (void)br.u(8);
    const ParseError e = expect_error([&] { (void)br.leb128(); }, "BR-0001");
    CHECK(e.bit_offset == 8);
    CHECK(br.bit_pos() == 8);
    BitReader empty(std::span<const uint8_t>{});
    expect_error([&] { (void)empty.leb128(); }, "BR-0001");
  }
}

TEST_CASE("br_leb128_overlong", "[bitreader][av1][fast]") {
  const Bytes eight(8, 0x80);
  SECTION("acceptance 2: with a sink, BR-0009 is recorded and the value is 0") {
    RecordingSink sink;
    BitReader br(eight);
    br.set_diagnostic_sink(&sink);
    CHECK(br.leb128() == 0);
    CHECK(br.bit_pos() == 64);
    REQUIRE(sink.warnings.size() == 1);
    CHECK(sink.warnings[0].code == "BR-0009");
    CHECK(sink.warnings[0].bit_offset == 56);  // the 8th byte
    CHECK(sink.warnings[0].message == "leb128 8th byte has continuation bit at bit 56");
  }
  SECTION("acceptance 2: without a sink, value 0 and no throw") {
    BitReader br(eight);
    uint64_t v = 1;
    CHECK_NOTHROW(v = br.leb128());
    CHECK(v == 0);
    CHECK(br.bit_pos() == 64);
  }
  SECTION("the continuation bit is ignored, the 56-bit value is kept, and the 9th byte is not read") {
    const Bytes b = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    RecordingSink sink;
    BitReader br(b);
    br.set_diagnostic_sink(&sink);
    CHECK(br.leb128() == (uint64_t{1} << 56) - 1);
    CHECK(br.bit_pos() == 64);
    CHECK(sink.warnings.size() == 1);
  }
}

TEST_CASE("br_uvlc", "[bitreader][av1][fast]") {
  struct Case {
    std::string bits;
    uint32_t value;
    uint64_t consumed;
  };
  const std::vector<Case> cases = {
      {"1", 0, 1}, {"010", 1, 3}, {"011", 2, 3}, {"00100", 3, 5}, {"00111", 6, 5}, {"0001000", 7, 7},
  };
  for (const auto& c : cases) {
    INFO(c.bits);
    const Bytes b = from_bits(c.bits);
    BitReader br(b);
    CHECK(br.uvlc() == c.value);
    CHECK(br.bit_pos() == c.consumed);
  }

  SECTION("31 leading zeros: largest coded value 2^32 - 2") {
    const Bytes b = from_bits(std::string(31, '0') + "1" + std::string(31, '1'));
    BitReader br(b);
    CHECK(br.uvlc() == 0xFFFFFFFEU);
    CHECK(br.bit_pos() == 63);
  }

  SECTION("32 leading zeros: (1<<32)-1 without reading the value bits") {
    // 32 zeros and the one bit, then the buffer ends: the value bits would not fit, and are not read.
    const Bytes b = from_bits(std::string(32, '0') + "1" + "0000000");
    BitReader br(b);
    CHECK(br.uvlc() == 0xFFFFFFFFU);
    CHECK(br.bit_pos() == 33);
  }

  SECTION("more than 32 leading zeros, across cache refills") {
    const Bytes b = from_bits(std::string(200, '0') + "1" + "1010");
    BitReader br(b);
    CHECK(br.uvlc() == 0xFFFFFFFFU);
    CHECK(br.bit_pos() == 201);
    CHECK(br.u(4) == 0b1010);
  }

  SECTION("unaligned start, prefix across a byte boundary") {
    const Bytes b = from_bits("111111 0001 101");
    BitReader br(b);
    br.skip_bits(6);
    CHECK(br.uvlc() == 7 + 0b101);
    CHECK(br.bit_pos() == 13);
  }

  SECTION("no one bit: BR-0001 and position unchanged") {
    const Bytes zeros(5, 0x00);
    BitReader br(zeros);
    (void)br.u(3);
    expect_error([&] { (void)br.uvlc(); }, "BR-0001");
    CHECK(br.bit_pos() == 3);
  }

  SECTION("value bits past the end: BR-0001 and position unchanged") {
    const Bytes b = from_bits("00000001");  // seven zeros, the one bit, no value bits
    BitReader br(b);
    expect_error([&] { (void)br.uvlc(); }, "BR-0001");
    CHECK(br.bit_pos() == 0);
  }
}

TEST_CASE("br_su_ns", "[bitreader][av1][fast]") {
  SECTION("acceptance 4: su(7) of 1000000 is -64") {
    const Bytes b = from_bits("1000000");
    BitReader br(b);
    CHECK(br.su(7) == -64);
    CHECK(br.bit_pos() == 7);
  }
  SECTION("su sign handling") {
    const Bytes b = from_bits("0111111 1111111 1 0");
    BitReader br(b);
    CHECK(br.su(7) == 63);
    CHECK(br.su(7) == -1);
    CHECK(br.su(1) == -1);
    CHECK(br.su(1) == 0);
    const Bytes ones(8, 0xFF);
    BitReader br64(ones);
    CHECK(br64.su(64) == -1);
  }
  SECTION("su(0) is an invalid width") {
    const Bytes b = {0xFF};
    BitReader br(b);
    expect_error([&] { (void)br.su(0); }, "BR-0003");
    expect_error([&] { (void)br.su(65); }, "BR-0003");
    CHECK(br.bit_pos() == 0);
  }

  SECTION("acceptance 3 and §7.2.7: ns(5), w = 3, m = 3") {
    struct Case {
      std::string bits;
      uint32_t value;
      uint64_t consumed;
    };
    const std::vector<Case> cases = {
        {"00", 0, 2}, {"01", 1, 2}, {"10", 2, 2}, {"110", 3, 3}, {"111", 4, 3},
    };
    for (const auto& c : cases) {
      INFO(c.bits);
      const Bytes b = from_bits(c.bits);
      BitReader br(b);
      CHECK(br.ns(5) == c.value);
      CHECK(br.bit_pos() == c.consumed);
    }
  }

  SECTION("ns covers [0, n) for every coding of small n") {
    for (uint32_t n = 1; n <= 64; ++n) {
      const auto w = static_cast<unsigned>(std::bit_width(n));
      std::vector<bool> seen(n, false);
      for (uint64_t pattern = 0; pattern < (uint64_t{1} << w); ++pattern) {
        // Left-align the w-bit pattern in two bytes.
        const auto word = static_cast<uint16_t>(pattern << (16 - w));
        const Bytes b = {static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word & 0xFF)};
        BitReader br(b);
        const uint32_t v = br.ns(n);
        REQUIRE(v < n);
        seen[v] = true;
      }
      for (uint32_t v = 0; v < n; ++v) {
        INFO("n=" << n << " v=" << v);
        CHECK(seen[v]);
      }
    }
  }

  SECTION("ns(1) reads no bits; ns of the largest range") {
    const Bytes b = {0xFF, 0xFF, 0xFF, 0xFF};
    BitReader br(b);
    CHECK(br.ns(1) == 0);
    CHECK(br.bit_pos() == 0);
    CHECK(br.ns(0xFFFFFFFFU) == 0xFFFFFFFEU);  // w = 32, m = 1, v = 2^31-1 >= m, extra = 1
    CHECK(br.bit_pos() == 32);
  }

  SECTION("ns(0) is BR-0006; a missing extra bit is BR-0001; position unchanged") {
    const Bytes b = from_bits("000000 11");
    BitReader br(b);
    expect_error([&] { (void)br.ns(0); }, "BR-0006");
    br.skip_bits(6);
    expect_error([&] { (void)br.ns(5); }, "BR-0001");  // v = 11 needs the extra bit, which is past the end
    CHECK(br.bit_pos() == 6);
    BitReader br2(b);
    br2.skip_bits(7);
    expect_error([&] { (void)br2.ns(5); }, "BR-0001");  // not even w-1 bits
    CHECK(br2.bit_pos() == 7);
  }
}

TEST_CASE("br_le", "[bitreader][av1][fast]") {
  const Bytes b = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
  SECTION("§7.2.7: le(4) of 01 02 03 04") {
    BitReader br(b);
    CHECK(br.le(4) == 0x04030201U);
    CHECK(br.bit_pos() == 32);
  }
  SECTION("le(1) and le(8)") {
    BitReader br(b);
    CHECK(br.le(1) == 0x01);
    CHECK(br.le(8) == 0x0908070605040302ULL);
  }
  SECTION("errors leave the position unchanged") {
    BitReader br(b);
    expect_error([&] { (void)br.le(0); }, "BR-0003");
    expect_error([&] { (void)br.le(9); }, "BR-0003");
    (void)br.u(1);
    expect_error([&] { (void)br.le(1); }, "BR-0004");  // le(n) requires byte alignment
    CHECK(br.bit_pos() == 1);
    br.seek_bits(48);
    expect_error([&] { (void)br.le(4); }, "BR-0001");
    CHECK(br.bit_pos() == 48);
  }
}

TEST_CASE("br_delta_q", "[bitreader][av1][fast]") {
  SECTION("§7.2.7 vectors") {
    const Bytes b = from_bits("0 1 1000000 1 0111111");
    BitReader br(b);
    CHECK(br.delta_q() == 0);
    CHECK(br.bit_pos() == 1);
    CHECK(br.delta_q() == -64);
    CHECK(br.bit_pos() == 9);
    CHECK(br.delta_q() == 63);
    CHECK(br.bit_pos() == 17);
  }
  SECTION("truncated su(7) leaves the delta_coded bit unread") {
    const Bytes b = from_bits("0100 0000");
    BitReader brc(b);
    brc.skip_bits(1);  // "100 0000": delta_coded = 1, then 6 bits left (su(7) needs 7)
    expect_error([&] { (void)brc.delta_q(); }, "BR-0001");
    CHECK(brc.bit_pos() == 1);
    BitReader empty(std::span<const uint8_t>{});
    expect_error([&] { (void)empty.delta_q(); }, "BR-0001");
  }
}

TEST_CASE("br_trailing_bits", "[bitreader][av1][fast]") {
  SECTION("§7.2.7: trailing_bits(8) of 80 is ok, of C0 is BR-0008") {
    const Bytes ok = {0x80};
    BitReader br(ok);
    br.trailing_bits(8);
    CHECK(br.bit_pos() == 8);
    const Bytes bad = {0xC0};
    BitReader brb(bad);
    const ParseError e = expect_error([&] { brb.trailing_bits(8); }, "BR-0008");
    CHECK(e.bit_offset == 1);
    CHECK(brb.bit_pos() == 0);
  }
  SECTION("missing trailing_one_bit") {
    const Bytes b = {0x40};
    BitReader br(b);
    const ParseError e = expect_error([&] { br.trailing_bits(8); }, "BR-0008");
    CHECK(e.bit_offset == 0);
    CHECK(br.bit_pos() == 0);
  }
  SECTION("unaligned start and long zero runs") {
    const Bytes b = from_bits("101" + std::string(200, '0'));
    BitReader br(b);
    (void)br.u(2);
    br.trailing_bits(201);
    CHECK(br.bit_pos() == 203);
    // A one bit deep inside the zero run is reported at its own position.
    Bytes c(30, 0x00);
    c[0] = 0x80;
    c[20] = 0x10;  // bit 163
    BitReader brc(c);
    const ParseError e = expect_error([&] { brc.trailing_bits(240); }, "BR-0008");
    CHECK(e.bit_offset == 163);
    CHECK(brc.bit_pos() == 0);
  }
  SECTION("trailing_bits(1) is the one bit alone") {
    const Bytes b = {0xFF};
    BitReader br(b);
    br.trailing_bits(1);
    CHECK(br.bit_pos() == 1);
  }
  SECTION("nb_bits beyond the end is BR-0001; nb_bits == 0 is BR-0006 (#85)") {
    const Bytes b = {0x80};
    BitReader br(b);
    expect_error([&] { br.trailing_bits(9); }, "BR-0001");
    expect_error([&] { br.trailing_bits(0); }, "BR-0006");
    CHECK(br.bit_pos() == 0);
  }
}

TEST_CASE("br_byte_alignment", "[bitreader][av1][fast]") {
  const Bytes b = from_bits("1000 0000 1100 0000");
  BitReader br(b);
  br.byte_alignment();  // already aligned: reads nothing
  CHECK(br.bit_pos() == 0);
  (void)br.u(1);
  br.byte_alignment();
  CHECK(br.bit_pos() == 8);
  (void)br.u(3);  // "110"
  br.byte_alignment();
  CHECK(br.bit_pos() == 16);

  const Bytes bad = from_bits("1001 0000");
  BitReader brb(bad);
  (void)brb.u(1);
  const ParseError e = expect_error([&] { brb.byte_alignment(); }, "BR-0008");
  CHECK(e.bit_offset == 3);
  CHECK(brb.bit_pos() == 1);
}

TEST_CASE("br_L_guard", "[bitreader][av1][fast]") {
  const Bytes b = {0xFF};
  BitReader br(b);
  (void)br.u(3);
  const ParseError e = expect_error([&] { br.L(1); }, "BR-0010");
  CHECK(e.bit_offset == 3);
  CHECK(std::string(e.what()) == "L(n) requires the AV1 symbol decoder");
  CHECK(br.bit_pos() == 3);
}
