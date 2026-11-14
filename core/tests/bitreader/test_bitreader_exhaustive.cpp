// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Differential and exhaustive tests of ovt::BitReader and the emulation-prevention mapping (WI-1.570 step 1,
// chapter 7 §7.2.7). The per-descriptor rows live in test_bitreader.cpp, test_bitreader_av1.cpp and
// test_bitreader_rbsp.cpp; this file writes bit strings with an independent encoder and reads them back, so that
// every descriptor is checked in sequence, at every alignment, at every buffer length and through EPB mapping.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"

namespace {

using ovt::BitReader;
using ovt::ParseError;
using Bytes = std::vector<uint8_t>;

// Independent bit writer (MSB first). Never shares code with the reader under test.
class BitWriter {
 public:
  void put(uint64_t value, unsigned n) {
    for (unsigned k = n; k > 0; --k) {
      bit(((value >> (k - 1U)) & 1U) != 0);
    }
  }
  void bit(bool b) {
    if (used_ % 8 == 0) {
      bytes_.push_back(0);
    }
    if (b) {
      bytes_.back() = static_cast<uint8_t>(bytes_.back() | (0x80U >> (used_ % 8)));
    }
    ++used_;
  }
  void ue(uint64_t code_num) {
    const auto v = code_num + 1;
    const auto width = static_cast<unsigned>(std::bit_width(v));
    put(0, width - 1);
    put(v, width);
  }
  void se(int64_t value) { ue(value > 0 ? (static_cast<uint64_t>(value) * 2) - 1 : static_cast<uint64_t>(-value) * 2); }
  void uvlc(uint32_t value) {  // AV1 §4.10.3 inverse; value < 2^32-1
    const auto v = static_cast<uint64_t>(value) + 1;
    const auto width = static_cast<unsigned>(std::bit_width(v));
    put(0, width - 1);
    put(v, width);
  }
  void ns(uint32_t n, uint32_t v) {  // AV1 §4.10.7 inverse
    const auto w = static_cast<unsigned>(std::bit_width(n));
    const uint32_t m = (1U << w) - n;
    if (v < m) {
      put(v, w - 1);
    } else {
      const uint32_t t = v + m;  // v = (x << 1) - m + extra  ->  x = (v + m) >> 1, extra = (v + m) & 1
      put(t >> 1U, w - 1);
      put(t & 1U, 1);
    }
  }
  void leb128(uint64_t value) {
    do {
      uint8_t byte = value & 0x7FU;
      value >>= 7U;
      if (value != 0) {
        byte |= 0x80U;
      }
      put(byte, 8);
    } while (value != 0);
  }
  void align_zero() {
    while (used_ % 8 != 0) {
      bit(false);
    }
  }
  [[nodiscard]] uint64_t bits() const { return used_; }
  [[nodiscard]] const Bytes& bytes() const { return bytes_; }

 private:
  Bytes bytes_;
  uint64_t used_ = 0;
};

unsigned ref_bit(const Bytes& b, uint64_t k) {
  return (b.at(k / 8) >> (7 - (k % 8))) & 1U;
}

// Runs fn and returns the ParseError code it throws, or "" when it does not throw.
template <typename Fn>
std::string error_code(Fn&& fn) {
  try {
    fn();
  } catch (const ParseError& e) {
    return e.code;
  }
  return {};
}

}  // namespace

TEST_CASE("br_exhaustive_ue", "[bitreader][exhaustive][fast]") {
  // Every codeNum below 2^17 and the boundaries up to the largest legal one (2^32-2), at the 8 alignments.
  std::vector<uint32_t> values(1U << 17);
  std::iota(values.begin(), values.end(), 0U);
  for (unsigned w = 17; w <= 32; ++w) {
    const uint64_t top = (uint64_t{1} << w) - 2;  // 2^w - 2 is the last codeNum with w-1 leading zeros
    for (const uint64_t k : {top - 1, top, top + 1}) {
      if (k <= 0xFFFFFFFEULL) {
        values.push_back(static_cast<uint32_t>(k));
      }
    }
  }
  values.push_back(0xFFFFFFFEU);
  for (unsigned start = 0; start < 8; ++start) {
    BitWriter w;
    w.put(0x2AU, start);  // an arbitrary prefix of `start` bits
    for (const uint32_t v : values) {
      w.ue(v);
    }
    w.put(0xFFU, 8);  // padding so that a final peek is legal
    BitReader br(w.bytes());
    br.skip_bits(start);
    for (const uint32_t v : values) {
      REQUIRE(br.ue() == v);
    }
  }
}

TEST_CASE("br_exhaustive_av1_descriptors", "[bitreader][exhaustive][fast]") {
  SECTION("uvlc") {
    std::vector<uint32_t> values(70000);
    std::iota(values.begin(), values.end(), 0U);
    for (unsigned w = 17; w < 32; ++w) {
      const auto top = static_cast<uint32_t>((uint64_t{1} << w) - 1);
      values.push_back(top - 1);
      values.push_back(top);
      values.push_back(top + 1);
    }
    values.push_back(0xFFFFFFFEU);
    BitWriter w;
    for (const uint32_t v : values) {
      w.uvlc(v);
    }
    w.put(0xFFU, 8);
    BitReader br(w.bytes());
    for (const uint32_t v : values) {
      REQUIRE(br.uvlc() == v);
    }
  }
  SECTION("ns(n) for every n up to 1100 and every value") {
    for (uint32_t n = 1; n <= 1100; ++n) {
      BitWriter w;
      for (uint32_t v = 0; v < n; ++v) {
        w.ns(n, v);
      }
      w.put(0xFFU, 8);
      BitReader br(w.bytes());
      for (uint32_t v = 0; v < n; ++v) {
        REQUIRE(br.ns(n) == v);
      }
    }
  }
  SECTION("leb128 for every length from 1 to 8 bytes") {
    std::vector<uint64_t> values = {0, 1, 0x7F, 0x80, 0x3FFF, 0x4000};
    for (unsigned bytes = 1; bytes <= 8; ++bytes) {
      const uint64_t top = (uint64_t{1} << (7 * bytes)) - 1;  // largest value of `bytes` bytes
      values.push_back(top);
      values.push_back(top - 1);
    }
    BitWriter w;
    for (const uint64_t v : values) {
      w.leb128(v);
    }
    BitReader br(w.bytes());
    for (const uint64_t v : values) {
      REQUIRE(br.leb128() == v);
    }
    CHECK(br.bits_left() == 0);
  }
}

namespace {

enum class Op : uint8_t { U, I, Ue, Se, Te, Uvlc, Su, Ns, Leb, B8, Skip };

struct Step {
  Op op = Op::U;
  uint64_t a = 0;  // width, cmax or n
  int64_t value = 0;
  uint64_t start = 0;
  uint64_t length = 0;
};

// The two's-complement value of the low `n` bits of `raw`.
int64_t sign_extend(uint64_t raw, unsigned n) {
  if (n == 64) {
    return static_cast<int64_t>(raw);
  }
  const uint64_t sign = uint64_t{1} << (n - 1U);
  return static_cast<int64_t>((raw & sign) != 0 ? raw - (sign << 1U) : raw);  // unsigned wrap-around, no overflow
}

// Writes one random descriptor with the independent encoder and returns what the reader must return.
Step write_step(BitWriter& w, std::mt19937_64& rng) {
  Step st{static_cast<Op>(rng() % 11), 0, 0, w.bits(), 0};
  if (st.op == Op::Leb) {
    w.align_zero();  // leb128 requires a byte-aligned reader
    st.start = w.bits();
  }
  switch (st.op) {
    case Op::U:
      st.a = rng() % 65;
      st.value = static_cast<int64_t>(st.a == 0 ? 0 : rng() >> (64 - st.a));
      w.put(static_cast<uint64_t>(st.value), static_cast<unsigned>(st.a));
      break;
    case Op::I:
      st.a = 1 + (rng() % 64);
      {
        const uint64_t raw = st.a == 64 ? rng() : (rng() >> (64 - st.a));
        w.put(raw, static_cast<unsigned>(st.a));
        st.value = sign_extend(raw, static_cast<unsigned>(st.a));
      }
      break;
    case Op::Ue:
      st.value = static_cast<int64_t>(rng() % 100000);
      w.ue(static_cast<uint64_t>(st.value));
      break;
    case Op::Se:
      st.value = static_cast<int64_t>(rng() % 200001) - 100000;
      w.se(st.value);
      break;
    case Op::Te:
      st.a = 1 + (rng() % 40);
      st.value = static_cast<int64_t>(rng() % (st.a + 1));
      if (st.a == 1) {
        w.bit(st.value == 0);  // te(1) is the inverted bit
      } else {
        w.ue(static_cast<uint64_t>(st.value));
      }
      break;
    case Op::Uvlc:
      st.value = static_cast<int64_t>(rng() % 3000000);
      w.uvlc(static_cast<uint32_t>(st.value));
      break;
    case Op::Su:
      st.a = 2 + (rng() % 31);
      {
        const uint64_t raw = rng() >> (64 - st.a);
        w.put(raw, static_cast<unsigned>(st.a));
        st.value = sign_extend(raw, static_cast<unsigned>(st.a));
      }
      break;
    case Op::Ns:
      st.a = 1 + (rng() % 2000);
      st.value = static_cast<int64_t>(rng() % st.a);
      w.ns(static_cast<uint32_t>(st.a), static_cast<uint32_t>(st.value));
      break;
    case Op::Leb:
      st.value = static_cast<int64_t>(rng() >> (8 + (rng() % 56)));
      w.leb128(static_cast<uint64_t>(st.value));
      break;
    case Op::B8:
      st.value = static_cast<int64_t>(rng() & 0xFFU);
      w.put(static_cast<uint64_t>(st.value), 8);
      break;
    case Op::Skip:
      st.a = rng() % 70;
      w.put(rng(), static_cast<unsigned>(st.a % 64));
      w.put(rng(), static_cast<unsigned>(st.a - (st.a % 64)));
      break;
  }
  st.length = w.bits() - st.start;
  return st;
}

void read_and_check(BitReader& br, const Step& st) {
  switch (st.op) {
    case Op::U:
      REQUIRE(br.peek_bits(static_cast<unsigned>(st.a)) == static_cast<uint64_t>(st.value));
      REQUIRE(br.u(static_cast<unsigned>(st.a)) == static_cast<uint64_t>(st.value));
      break;
    case Op::I:
      REQUIRE(br.i(static_cast<unsigned>(st.a)) == st.value);
      break;
    case Op::Ue:
      REQUIRE(br.ue() == static_cast<uint32_t>(st.value));
      break;
    case Op::Se:
      REQUIRE(br.se() == static_cast<int32_t>(st.value));
      break;
    case Op::Te:
      REQUIRE(br.te(static_cast<uint32_t>(st.a)) == static_cast<uint32_t>(st.value));
      break;
    case Op::Uvlc:
      REQUIRE(br.uvlc() == static_cast<uint32_t>(st.value));
      break;
    case Op::Su:
      REQUIRE(br.su(static_cast<unsigned>(st.a)) == st.value);
      break;
    case Op::Ns:
      REQUIRE(br.ns(static_cast<uint32_t>(st.a)) == static_cast<uint32_t>(st.value));
      break;
    case Op::Leb:
      REQUIRE(br.leb128() == static_cast<uint64_t>(st.value));
      break;
    case Op::B8:
      REQUIRE(br.b8() == static_cast<uint8_t>(st.value));
      break;
    case Op::Skip:
      br.skip_bits(st.a);
      break;
  }
}

}  // namespace

TEST_CASE("br_exhaustive_mixed_program", "[bitreader][exhaustive][fast]") {
  // A random sequence of every descriptor is written with the independent encoder and read back; after each read the
  // position and span_since() must equal the number of bits written.
  std::mt19937_64 rng(0x570U);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seed keeps the test deterministic
  for (int round = 0; round < 400; ++round) {
    BitWriter w;
    std::vector<Step> steps;
    const auto count = 1 + (rng() % 60);
    steps.reserve(count);
    for (uint64_t k = 0; k < count; ++k) {
      steps.push_back(write_step(w, rng));
    }
    BitReader br(w.bytes());
    for (const Step& st : steps) {
      if (st.op == Op::Leb) {
        br.seek_bits(st.start);  // the encoder padded to a byte boundary
      }
      REQUIRE(br.bit_pos() == st.start);
      const auto mark = br.mark();
      read_and_check(br, st);
      REQUIRE(br.bit_pos() == st.start + st.length);
      const ovt::BitSpan span = br.span_since(mark);
      REQUIRE(span.bit_offset == st.start);
      REQUIRE(span.bit_length == st.length);
    }
    CHECK(br.bits_left() < 8);  // only the zero padding of the last byte is left
  }
}

namespace {

uint64_t ref_u(const Bytes& buf, uint64_t pos, unsigned n) {
  uint64_t v = 0;
  for (unsigned k = 0; k < n; ++k) {
    v = (v << 1U) | ref_bit(buf, pos + k);
  }
  return v;
}

// Every width at one position of one buffer: a read that does not fit throws BR-0001 and leaves the cursor and a
// following peek untouched, a read that fits returns the reference value.
void check_widths_at(const Bytes& buf, uint64_t pos) {
  const uint64_t left = (buf.size() * 8) - pos;
  for (unsigned n = 0; n <= 64; ++n) {
    BitReader br(buf);
    br.seek_bits(pos);
    if (n <= left) {
      REQUIRE(br.u(n) == ref_u(buf, pos, n));
      REQUIRE(br.bit_pos() == pos + n);
    } else {
      REQUIRE(error_code([&] { (void)br.u(n); }) == "BR-0001");
      REQUIRE(br.bit_pos() == pos);
      REQUIRE(error_code([&] { (void)br.peek_bits(n); }) == "BR-0001");
    }
  }
}

void check_position_api_at(const Bytes& buf, uint64_t pos) {
  const uint64_t total = buf.size() * 8;
  const uint64_t left = total - pos;
  BitReader br(buf);
  REQUIRE(error_code([&] { br.seek_bits(total + 1 + pos); }) == "BR-0002");
  br.seek_bits(pos);
  REQUIRE(br.bits_left() == left);
  REQUIRE(br.byte_aligned() == (pos % 8 == 0));
  REQUIRE(error_code([&] { br.skip_bits(left + 1); }) == "BR-0001");
  REQUIRE(br.bit_pos() == pos);
}

}  // namespace

TEST_CASE("br_exhaustive_boundaries", "[bitreader][exhaustive][fast]") {
  // For every buffer length 0..17 bytes and every bit position, at every width.
  std::mt19937 rng(570);  // NOLINT(cert-msc32-c,cert-msc51-cpp)
  for (size_t size = 0; size <= 17; ++size) {
    Bytes buf(size);
    for (auto& b : buf) {
      b = static_cast<uint8_t>(rng() & 0xFFU);
    }
    for (uint64_t pos = 0; pos <= size * 8; ++pos) {
      check_widths_at(buf, pos);
      check_position_api_at(buf, pos);
    }
  }
}

TEST_CASE("br_exhaustive_more_rbsp_data", "[bitreader][exhaustive][fast]") {
  // §7.2 more_rbsp_data(): true while a one bit remains after the current position other than the last one in the
  // buffer. The reference scans for the last set bit. Every 1- and 2-byte buffer, every position.
  const auto reference = [](const Bytes& buf, uint64_t pos) {
    uint64_t last = 0;
    bool any = false;
    for (uint64_t k = 0; k < buf.size() * 8; ++k) {
      if (ref_bit(buf, k) != 0) {
        last = k;
        any = true;
      }
    }
    return any && pos < last;
  };
  for (unsigned v = 0; v < 65536; ++v) {
    const Bytes buf = {static_cast<uint8_t>(v >> 8U), static_cast<uint8_t>(v & 0xFFU)};
    for (uint64_t pos = 0; pos <= 16; ++pos) {
      BitReader br(buf);
      br.seek_bits(pos);
      REQUIRE(br.more_rbsp_data() == reference(buf, pos));
    }
  }
}

TEST_CASE("br_exhaustive_epb_mapping_through_the_reader", "[bitreader][rbsp][exhaustive][fast]") {
  // A random RBSP (biased to 00/01/02/03) gets emulation-prevention bytes from an independent inserter; the RBSP read
  // back by rbsp_from_nal equals the original and the raw size exceeds it by exactly the number of EPBs (acceptance
  // test 2 of WI-1.570). Then every byte of the RBSP is read through a BitReader and its span is mapped to the raw
  // bytes: the raw bit at the mapped position equals the RBSP bit.
  std::mt19937_64 rng(0xE9B);  // NOLINT(cert-msc32-c,cert-msc51-cpp)
  size_t with_epb = 0;
  for (int round = 0; round < 600; ++round) {
    Bytes rbsp(2 + (rng() % 300));
    for (auto& b : rbsp) {
      const auto k = static_cast<unsigned>(rng() % 10);
      b = k < 6 ? 0x00 : (k < 9 ? static_cast<uint8_t>(k - 5) : static_cast<uint8_t>(rng() & 0xFFU));
    }
    rbsp[0] = 0x65;                                                // header byte, never zero
    rbsp.back() = rbsp.back() == 0 ? uint8_t{0x80} : rbsp.back();  // a conformant RBSP ends with a non-zero byte
    Bytes raw;
    std::vector<uint32_t> inserted;
    unsigned zeros = 0;
    for (const uint8_t b : rbsp) {
      if (zeros >= 2 && b <= 3) {
        inserted.push_back(static_cast<uint32_t>(raw.size()));
        raw.push_back(3);
        zeros = 0;
      }
      raw.push_back(b);
      zeros = b == 0 ? zeros + 1 : 0;
    }
    const ovt::Rbsp out = ovt::rbsp_from_nal(raw);
    REQUIRE(out.rbsp == rbsp);
    REQUIRE(out.epb_positions == inserted);
    REQUIRE(raw.size() - out.rbsp.size() == out.epb_positions.size());
    with_epb += inserted.empty() ? size_t{0} : size_t{1};

    BitReader br(out.rbsp);
    br.skip_bytes(1);  // NAL header
    while (br.bits_left() >= 8) {
      const auto mark = br.mark();
      const uint64_t value = br.u(static_cast<unsigned>(1 + (rng() % 8)));
      const ovt::BitSpan span = br.span_since(mark);
      const uint64_t raw_bit = ovt::rbsp_bit_to_raw_bit(span.bit_offset, out.epb_positions);
      // the first bit of the span must be the same bit in both representations
      REQUIRE(ref_bit(raw, raw_bit) == ref_bit(out.rbsp, span.bit_offset));
      (void)value;
    }
  }
  CHECK(with_epb > 100);  // the generator really produces EPBs
}
