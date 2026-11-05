// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of ovt::BitReader, H.264/H.265 rows of chapter 7 §7.2.7 (WI-1.102). The AV1 rows belong to
// WI-1.103 (test_bitreader_av1.cpp).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

// Naive reference: bit k of the buffer, MSB first.
unsigned ref_bit(const Bytes& b, uint64_t k) {
  return (b.at(k / 8) >> (7 - (k % 8))) & 1U;
}

uint64_t ref_u(const Bytes& b, uint64_t pos, unsigned n) {
  uint64_t v = 0;
  for (unsigned k = 0; k < n; ++k) {
    v = (v << 1U) | ref_bit(b, pos + k);
  }
  return v;
}

Bytes random_bytes(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<unsigned> dist(0, 255);
  Bytes out(n);
  for (auto& byte : out) {
    byte = static_cast<uint8_t>(dist(rng));
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

struct RecordingSink final : BitReader::DiagnosticSink {
  std::vector<std::string> codes;
  void reader_warning(std::string_view code, std::string_view /*message*/, uint64_t /*bit_offset*/) override {
    codes.emplace_back(code);
  }
};

}  // namespace

TEST_CASE("br_parse_error", "[bitreader][fast]") {
  const ParseError e("BR-0001", "some message", 42);
  CHECK(e.code == "BR-0001");
  CHECK(e.bit_offset == 42);
  CHECK(std::string(e.what()) == "some message");
}

TEST_CASE("br_u_all_widths", "[bitreader][fast]") {
  const Bytes buf = random_bytes(1024, 0x0b17U);
  const uint64_t total = buf.size() * 8;

  SECTION("every width from every alignment") {
    for (unsigned n = 1; n <= 64; ++n) {
      for (unsigned start = 0; start < 8; ++start) {
        BitReader br(buf);
        br.skip_bits(start);
        uint64_t pos = start;
        while (br.bits_left() >= n) {
          REQUIRE(br.peek_bits(n) == ref_u(buf, pos, n));
          REQUIRE(br.u(n) == ref_u(buf, pos, n));
          pos += n;
          REQUIRE(br.bit_pos() == pos);
        }
        CHECK(br.bits_left() < n);
      }
    }
  }

  SECTION("random width sequence, f and u agree") {
    std::mt19937 rng(7);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds keep the tests deterministic
    std::uniform_int_distribution<unsigned> width(0, 64);
    BitReader br(buf.data(), buf.size());
    uint64_t pos = 0;
    while (true) {
      const unsigned n = width(rng);
      if (n > total - pos) {
        break;
      }
      const uint64_t expected = ref_u(buf, pos, n);
      REQUIRE(((n % 2) != 0 ? br.u(n) : br.f(n)) == expected);
      pos += n;
    }
    CHECK(br.bit_pos() == pos);
  }

  SECTION("u(0) returns 0 and reads nothing, also at the end") {
    const Bytes one = {0xFF};
    BitReader br(one);
    CHECK(br.u(0) == 0);
    CHECK(br.bit_pos() == 0);
    br.skip_bits(8);
    CHECK(br.u(0) == 0);
    CHECK(br.bit_pos() == 8);
  }

  SECTION("b8 reads a whole byte at any alignment") {
    const Bytes two = {0xA5, 0x3C};
    BitReader br(two);
    CHECK(br.b8() == 0xA5);
    br.seek_bits(4);
    CHECK(br.b8() == 0x53);
  }

  SECTION("width above 64 is BR-0003") {
    BitReader br(buf);
    expect_error([&] { (void)br.u(65); }, "BR-0003");
    expect_error([&] { (void)br.peek_bits(65); }, "BR-0003");
    CHECK(br.bit_pos() == 0);
  }
}

TEST_CASE("br_i_signed", "[bitreader][fast]") {
  {
    const Bytes b = from_bits("111");
    BitReader br(b);
    CHECK(br.i(3) == -1);
  }
  {
    const Bytes b = from_bits("100 011");
    BitReader br(b);
    CHECK(br.i(3) == -4);
    CHECK(br.i(3) == 3);
  }
  const Bytes ones(8, 0xFF);
  for (unsigned n = 1; n <= 64; ++n) {
    BitReader br(ones);
    CHECK(br.i(n) == -1);
  }
  const Bytes msb = {0x80, 0, 0, 0, 0, 0, 0, 0};
  for (unsigned n = 1; n <= 64; ++n) {
    BitReader br(msb);
    const int64_t expected = n == 64 ? INT64_MIN : -(int64_t{1} << (n - 1));
    CHECK(br.i(n) == expected);
  }
  {
    const Bytes b = {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    BitReader br(b);
    CHECK(br.i(64) == INT64_MAX);
  }
  BitReader br(ones);
  expect_error([&] { (void)br.i(0); }, "BR-0003");
  expect_error([&] { (void)br.i(65); }, "BR-0003");
  CHECK(br.bit_pos() == 0);
}

TEST_CASE("br_ue_table", "[bitreader][fast]") {
  // H.264 Table 9-2 bit strings → codeNum.
  const std::array<std::pair<const char*, uint32_t>, 8> rows = {
      {{"1", 0}, {"010", 1}, {"011", 2}, {"00100", 3}, {"00101", 4}, {"00110", 5}, {"00111", 6}, {"0001000", 7}}};
  for (const auto& [bits, code_num] : rows) {
    const Bytes b = from_bits(bits);
    BitReader br(b);
    CHECK(br.ue() == code_num);
    CHECK(br.bit_pos() == std::string_view(bits).size());
  }
  // All eight codes back to back.
  const Bytes all = from_bits("1 010 011 00100 00101 00110 00111 0001000");
  BitReader br(all);
  for (uint32_t k = 0; k < 8; ++k) {
    CHECK(br.ue() == k);
  }
  // The largest codeNum: 31 leading zeros, a one and 31 ones = 2^32 - 2.
  const Bytes big = from_bits(std::string(31, '0') + "1" + std::string(31, '1'));
  BitReader brb(big);
  CHECK(brb.ue() == 4294967294U);
  CHECK(brb.bit_pos() == 63);
}

TEST_CASE("br_se_table", "[bitreader][fast]") {
  // Table 9-3: codeNum 0..4 → 0, 1, -1, 2, -2.
  const Bytes b = from_bits("1 010 011 00100 00101");
  BitReader br(b);
  CHECK(br.se() == 0);
  CHECK(br.se() == 1);
  CHECK(br.se() == -1);
  CHECK(br.se() == 2);
  CHECK(br.se() == -2);
  // codeNum 2^32-2 → -(2^31-1) and 2^32-3 → 2^31-1, with no overflow.
  const Bytes neg = from_bits(std::string(31, '0') + "1" + std::string(31, '1'));
  BitReader brn(neg);
  CHECK(brn.se() == -2147483647);
  const Bytes pos = from_bits(std::string(31, '0') + "1" + std::string(30, '1') + "0");
  BitReader brp(pos);
  CHECK(brp.se() == 2147483647);
}

TEST_CASE("br_te", "[bitreader][fast]") {
  {
    const Bytes b = from_bits("0 1");
    BitReader br(b);
    CHECK(br.te(1) == 1);  // one bit, inverted
    CHECK(br.bit_pos() == 1);
    CHECK(br.te(1) == 0);
    CHECK(br.bit_pos() == 2);
  }
  {
    const Bytes b = from_bits("00111 1 010");
    BitReader br(b);
    CHECK(br.te(5) == 6);
    CHECK(br.te(2) == 0);
    CHECK(br.te(5) == 1);
  }
  const Bytes b = from_bits("1");
  BitReader br(b);
  expect_error([&] { (void)br.te(0); }, "BR-0006");
  CHECK(br.bit_pos() == 0);
}

TEST_CASE("br_me", "[bitreader][fast]") {
  // Every Table 9-4 column is a permutation of the coded_block_pattern range.
  for (const uint32_t cat : {0U, 1U, 2U, 3U}) {
    const uint32_t count = (cat == 1 || cat == 2) ? 48 : 16;
    for (const bool intra : {true, false}) {
      std::set<uint32_t> seen;
      for (uint32_t k = 0; k < count; ++k) {
        // ue() code for codeNum k: (k+1) in binary, preceded by floor(log2(k+1)) zeros.
        const uint32_t v = k + 1;
        std::string bits;
        for (uint32_t t = v; t > 1; t >>= 1U) {
          bits += '0';
        }
        for (int bit = std::bit_width(v) - 1; bit >= 0; --bit) {
          bits += ((v >> static_cast<unsigned>(bit)) & 1U) != 0 ? '1' : '0';
        }
        const Bytes b = from_bits(bits);
        BitReader br(b);
        seen.insert(br.me(cat, intra));
      }
      CHECK(seen.size() == count);
      CHECK(*seen.rbegin() == count - 1);
    }
  }
  // Spot values of Table 9-4: codeNum 0 and 1.
  const Bytes b = from_bits("1 010 1 010 1 1 1 1");
  BitReader br(b);
  CHECK(br.me(1, true) == 47);
  CHECK(br.me(1, true) == 31);
  CHECK(br.me(2, false) == 0);
  CHECK(br.me(2, false) == 16);
  CHECK(br.me(0, true) == 15);
  CHECK(br.me(3, false) == 0);
  // codeNum 16 is out of range for ChromaArrayType 0/3; codeNum 48 for 1/2.
  const Bytes c16 = from_bits("000010001");
  BitReader br16(c16);
  expect_error([&] { (void)br16.me(0, true); }, "BR-0011");
  CHECK(br16.bit_pos() == 0);
  const Bytes c48 = from_bits("00000110001");
  BitReader br48(c48);
  expect_error([&] { (void)br48.me(1, false); }, "BR-0011");
  CHECK(br48.bit_pos() == 0);
  expect_error([&] { (void)br48.me(4, false); }, "BR-0006");
}

TEST_CASE("br_ue_too_long", "[bitreader][fast]") {
  {
    const Bytes zeros(4, 0x00);  // exactly 32 zero bits
    BitReader br(zeros);
    const ParseError e = expect_error([&] { (void)br.ue(); }, "BR-0005");
    CHECK(e.bit_offset == 0);
    CHECK(br.bit_pos() == 0);
  }
  {
    const Bytes zeros = {0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00};  // 32 zeros, then a one
    BitReader br(zeros);
    expect_error([&] { (void)br.se(); }, "BR-0005");
    CHECK(br.bit_pos() == 0);
  }
  {
    const Bytes zeros(3, 0x00);  // 24 zero bits, then the end
    BitReader br(zeros);
    expect_error([&] { (void)br.ue(); }, "BR-0001");
    CHECK(br.bit_pos() == 0);
  }
}

TEST_CASE("br_bounds", "[bitreader][fast]") {
  SECTION("acceptance: u(2) with one bit left") {
    const Bytes b = {0xFF, 0xFF};
    BitReader br(b);
    br.skip_bits(br.size_bits() - 1);
    const uint64_t before = br.bit_pos();
    const ParseError e = expect_error([&] { (void)br.u(2); }, "BR-0001");
    CHECK(e.bit_offset == br.size_bits() - 1);
    CHECK(br.bit_pos() == before);
  }

  SECTION("every read method one past the end leaves bit_pos() unchanged") {
    // A ones buffer; ue/se/te/me get zeros so that their prefix runs into the end.
    const Bytes ones(8, 0xFF);
    const Bytes zeros(4, 0x00);
    struct Case {
      const char* name;
      const Bytes* buf;
      uint64_t left;  // bits left when the call is made
      std::function<void(BitReader&)> call;
    };
    const std::vector<Case> cases = {
        {"u", &ones, 4, [](BitReader& r) { (void)r.u(5); }},
        {"u(64)", &ones, 63, [](BitReader& r) { (void)r.u(64); }},
        {"f", &ones, 4, [](BitReader& r) { (void)r.f(5); }},
        {"b8", &ones, 7, [](BitReader& r) { (void)r.b8(); }},
        {"i", &ones, 4, [](BitReader& r) { (void)r.i(5); }},
        {"peek_bits", &ones, 4, [](BitReader& r) { (void)r.peek_bits(5); }},
        {"peek_bit", &ones, 0, [](BitReader& r) { (void)r.peek_bit(); }},
        {"skip_bits", &ones, 4, [](BitReader& r) { r.skip_bits(5); }},
        {"skip_bytes", &ones, 8, [](BitReader& r) { r.skip_bytes(2); }},
        {"ue", &zeros, 3, [](BitReader& r) { (void)r.ue(); }},
        {"se", &zeros, 3, [](BitReader& r) { (void)r.se(); }},
        {"te", &zeros, 3, [](BitReader& r) { (void)r.te(7); }},
        {"te(1)", &zeros, 0, [](BitReader& r) { (void)r.te(1); }},
        {"me", &zeros, 3, [](BitReader& r) { (void)r.me(1, true); }},
        {"st", &ones, 8, [](BitReader& r) { (void)r.st(); }},
        {"expect_f", &ones, 4, [](BitReader& r) { r.expect_f(5, 31, "x"); }},
        {"rbsp_trailing_bits", &ones, 0, [](BitReader& r) { r.rbsp_trailing_bits(); }},
        {"bytes", &ones, 64, [](BitReader& r) { (void)r.bytes(6, 3); }},
    };
    for (const auto& c : cases) {
      INFO(c.name);
      BitReader br(*c.buf);
      br.skip_bits(br.size_bits() - c.left);
      const uint64_t before = br.bit_pos();
      expect_error([&] { c.call(br); }, "BR-0001");
      CHECK(br.bit_pos() == before);
    }
  }

  SECTION("ue suffix past the end") {
    const Bytes b = from_bits("0001");  // prefix of codeNum 7..14, suffix bits are padding then end
    BitReader br(b);
    br.skip_bits(4);
    (void)br.u(1);  // now at bit 5: "000" then end
    const uint64_t before = br.bit_pos();
    expect_error([&] { (void)br.ue(); }, "BR-0001");
    CHECK(br.bit_pos() == before);
    const Bytes c = {0x00, 0x01};  // 15 zeros, a one, no suffix
    BitReader brc(c);
    expect_error([&] { (void)brc.ue(); }, "BR-0001");
    CHECK(brc.bit_pos() == 0);
  }

  SECTION("empty buffer") {
    BitReader br(std::span<const uint8_t>{});
    CHECK(br.size_bits() == 0);
    CHECK(br.u(0) == 0);
    expect_error([&] { (void)br.u(1); }, "BR-0001");
    CHECK_FALSE(br.more_rbsp_data());
    CHECK_FALSE(br.more_data_in_byte_stream());
    BitReader brp(nullptr, 0);
    CHECK(brp.bits_left() == 0);
  }
}

TEST_CASE("br_more_rbsp_data", "[bitreader][fast]") {
  {
    const Bytes b = {0x80};
    BitReader br(b);
    CHECK_FALSE(br.more_rbsp_data());  // the stop bit is the next bit
  }
  {
    const Bytes b = {0x00, 0x80};
    BitReader br(b);
    CHECK(br.more_rbsp_data());
    br.skip_bits(7);
    CHECK(br.more_rbsp_data());
    br.skip_bits(1);
    CHECK_FALSE(br.more_rbsp_data());
  }
  {
    const Bytes b = {0xC0};
    BitReader br(b);
    CHECK(br.more_rbsp_data());
    br.skip_bits(1);
    CHECK_FALSE(br.more_rbsp_data());
  }
  {
    const Bytes b = {0xC0, 0x00, 0x00};  // cabac_zero_words after the trailing bits
    BitReader br(b);
    CHECK(br.more_rbsp_data());
    br.skip_bits(1);
    CHECK_FALSE(br.more_rbsp_data());
    br.rbsp_trailing_bits();
    CHECK(br.unread_bytes_after_trailing() == 2);
  }
  {
    const Bytes b = {0x00};  // no stop bit at all
    BitReader br(b);
    CHECK_FALSE(br.more_rbsp_data());
  }
  {
    const Bytes b = {0x12, 0x34, 0x01};
    BitReader br(b);
    br.skip_bits(23);
    CHECK_FALSE(br.more_rbsp_data());
    br.seek_bits(22);
    CHECK(br.more_rbsp_data());
  }
}

TEST_CASE("br_rbsp_trailing_bits", "[bitreader][fast]") {
  {
    const Bytes b = {0x80};
    BitReader br(b);
    br.rbsp_trailing_bits();
    CHECK(br.bit_pos() == 8);
    CHECK(br.byte_aligned());
  }
  {
    const Bytes b = {0x40};
    BitReader br(b);
    const ParseError e = expect_error([&] { br.rbsp_trailing_bits(); }, "BR-0008");
    CHECK(e.bit_offset == 0);
    CHECK(br.bit_pos() == 0);
  }
  {
    const Bytes b = {0xA0, 0xFF};  // "10" then stop bit at bit 2, then zeros
    BitReader br(b);
    CHECK(br.u(2) == 2);
    br.rbsp_trailing_bits();
    CHECK(br.bit_pos() == 8);
  }
  {
    const Bytes b = {0xA4};  // "10 1 00 1 00": a one among the alignment bits
    BitReader br(b);
    (void)br.u(2);
    const ParseError e = expect_error([&] { br.rbsp_trailing_bits(); }, "BR-0008");
    CHECK(e.bit_offset == 5);
    CHECK(br.bit_pos() == 2);
  }
}

TEST_CASE("br_st", "[bitreader][fast]") {
  {
    const Bytes b = {'a', 'b', 'c', 0x00, 0x42};
    BitReader br(b);
    CHECK(br.st() == "abc");
    CHECK(br.bit_pos() == 32);
    CHECK(br.b8() == 0x42);
  }
  {
    const Bytes b = {0x00};
    BitReader br(b);
    CHECK(br.st().empty());
    CHECK(br.bit_pos() == 8);
  }
  {
    const Bytes b = {'a', 'b', 'c'};
    BitReader br(b);
    expect_error([&] { (void)br.st(); }, "BR-0001");
    CHECK(br.bit_pos() == 0);
  }
  {
    const Bytes b = {'a', 0x00};
    BitReader br(b);
    br.skip_bits(1);
    expect_error([&] { (void)br.st(); }, "BR-0004");
    CHECK(br.bit_pos() == 1);
  }
  {
    Bytes b(65535, 'x');
    b.push_back(0x00);
    BitReader br(b);
    CHECK(br.st().size() == 65535);
  }
  {
    Bytes b(65536, 'x');
    b.push_back(0x00);
    BitReader br(b);
    expect_error([&] { (void)br.st(); }, "BR-0007");
    CHECK(br.bit_pos() == 0);
  }
}

TEST_CASE("br_expect_f", "[bitreader][fast]") {
  const Bytes b = from_bits("1010 0110");
  BitReader br(b);
  br.expect_f(4, 0xA, "forbidden_zero_bits");
  CHECK(br.bit_pos() == 4);
  const ParseError e = expect_error([&] { br.expect_f(4, 0x7, "reserved_bits"); }, "SYN-GEN-0003");
  CHECK(e.bit_offset == 4);
  CHECK(std::string(e.what()) == "reserved_bits: forbidden/reserved value 6");
  CHECK(br.bit_pos() == 4);
}

TEST_CASE("br_position", "[bitreader][fast]") {
  const Bytes b = {0x01, 0x02, 0x03, 0x04, 0x05};
  BitReader br(b);
  CHECK(br.size_bits() == 40);
  CHECK(br.byte_aligned());
  const BitReader::Mark m = br.mark();
  br.skip_bits(3);
  CHECK_FALSE(br.byte_aligned());
  CHECK(br.bits_left() == 37);
  const ovt::BitSpan s = br.span_since(m);
  CHECK(s.bit_offset == 0);
  CHECK(s.bit_length == 3);
  expect_error([&] { (void)br.remaining_bytes(); }, "BR-0004");
  expect_error([&] { br.skip_bytes(1); }, "BR-0004");
  CHECK(br.unread_bytes_after_trailing() == 4);

  br.seek_bits(16);
  CHECK(br.remaining_bytes().size() == 3);
  CHECK(br.remaining_bytes()[0] == 0x03);
  br.skip_bytes(1);
  CHECK(br.bit_pos() == 24);
  CHECK(br.peek_bits(8) == 0x04);
  CHECK_FALSE(br.peek_bit());
  CHECK(br.bit_pos() == 24);

  CHECK(br.bytes(1, 2)[1] == 0x03);
  CHECK(br.bytes(5, 0).empty());
  expect_error([&] { (void)br.bytes(6, 0); }, "BR-0001");

  br.seek_bits(40);
  CHECK(br.bits_left() == 0);
  CHECK(br.remaining_bytes().empty());
  CHECK(br.unread_bytes_after_trailing() == 0);
  expect_error([&] { br.seek_bits(41); }, "BR-0002");
  CHECK(br.bit_pos() == 40);
  br.seek_bits(5);
  CHECK(br.u(3) == 1);

  // span_since with a mark ahead of the position has length 0.
  CHECK(br.span_since(30).bit_length == 0);
}

TEST_CASE("br_sink_attach", "[bitreader][fast]") {
  const Bytes buf = random_bytes(256, 99);
  RecordingSink sink;
  BitReader with(buf);
  BitReader without(buf);
  CHECK(with.diagnostic_sink() == nullptr);
  with.set_diagnostic_sink(&sink);
  CHECK(with.diagnostic_sink() == &sink);
  with.set_diagnostic_sink(nullptr);
  CHECK(with.diagnostic_sink() == nullptr);
  with.set_diagnostic_sink(&sink);  // reattach for the comparison below, then compare with no sink
  std::mt19937 rng(3);              // NOLINT(cert-msc32-c,cert-msc51-cpp)
  std::uniform_int_distribution<unsigned> width(1, 24);
  while (with.bits_left() > 128) {
    const unsigned n = width(rng);
    REQUIRE(with.u(n) == without.u(n));
    REQUIRE(with.bit_pos() == without.bit_pos());
  }
  with.set_diagnostic_sink(nullptr);
  CHECK(with.diagnostic_sink() == nullptr);
  while (with.bits_left() > 0) {
    REQUIRE(with.u(1) == without.u(1));
  }
  CHECK(sink.codes.empty());
}
