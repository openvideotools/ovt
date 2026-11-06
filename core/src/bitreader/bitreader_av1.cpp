// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::BitReader: the AV1 descriptors of chapter 7 §7.2.2 (AV1 block, WI-1.103): leb128, uvlc, su, ns, le,
// delta_q, trailing_bits, byte_alignment and the L(n) guard. Formulas follow the §7.2.2 comments, which cite
// AV1 §4.10 and §5.3.4/§5.3.5/§5.9.13.
//
// Same rules as bitreader.cpp: every read checks its width and bounds before it moves the cursor, and every
// method that throws leaves bit_pos() where it was (reads run on a copy of the cursor that is committed only
// on success). Input bytes are reached only through the cursor helpers.

#include <bit>
#include <cstdint>
#include <string>

#include "ovt/cxx/bitreader.hpp"

namespace ovt {

namespace {

constexpr unsigned kLeb128MaxBytes = 8;  // AV1 §4.10.5: i < 8
constexpr unsigned kUvlcMaxLeadingZeros = 32;
constexpr unsigned kLeMaxBytes = 8;
constexpr unsigned kDeltaQBits = 1 + 6;  // AV1 §5.9.13: su(1+6)
constexpr unsigned kZeroChunk = 57;      // trailing zero bits are checked this many at a time

std::string str(uint64_t v) {
  return std::to_string(v);
}

// The callers below compute their own widths, so the overflow guard of bitreader.cpp is not needed here.
[[noreturn]] void throw_bounds(uint64_t n_bits, uint64_t at_bit, uint64_t size_bits) {
  throw ParseError(
      "BR-0001",
      "read of " + str(n_bits) + " bits at bit " + str(at_bit) + " exceeds unit size (" + str(size_bits) + " bits)",
      at_bit);
}

[[noreturn]] void throw_width(unsigned n, uint64_t at_bit) {
  throw ParseError("BR-0003", "invalid descriptor width " + str(n), at_bit);
}

[[noreturn]] void throw_range(const char* what, uint64_t at_bit) {
  throw ParseError("BR-0006", std::string("invalid range argument for ") + what, at_bit);
}

[[noreturn]] void throw_trailing(const char* expected, uint64_t at_bit, unsigned got) {
  throw ParseError("BR-0008", std::string("expected ") + expected + " at bit " + str(at_bit) + ", got " + str(got),
                   at_bit);
}

}  // namespace

uint64_t BitReader::leb128() {
  // AV1 §4.10.5:
  //   value = 0
  //   for (i = 0; i < 8; i++) { leb128_byte = f(8); value |= ((leb128_byte & 0x7f) << (i*7));
  //                             if (!(leb128_byte & 0x80)) break; }
  const uint64_t start = bit_pos();
  Cursor c = cur_;
  uint64_t value = 0;
  for (unsigned idx = 0; idx < kLeb128MaxBytes; ++idx) {
    const uint64_t byte_at = pos_of(c);
    if (size_bits() - byte_at < 8) {
      throw_bounds((uint64_t{idx} + 1) * 8, start, size_bits());
    }
    const uint64_t byte = take(c, 8);
    value |= (byte & 0x7FU) << (idx * 7);
    if ((byte & 0x80U) == 0) {
      cur_ = c;
      return value;
    }
    if (idx == kLeb128MaxBytes - 1) {
      // The 8th byte still continues: a conformance violation, not a read error (table 7.2.4, BR-0009).
      // The bit is ignored and the 56-bit value is returned. The warning points at the 8th byte.
      cur_ = c;
      warn("BR-0009", "leb128 8th byte has continuation bit at bit " + str(byte_at), byte_at);
    }
  }
  return value;
}

uint32_t BitReader::uvlc() {
  // AV1 §4.10.3:
  //   leadingZeros = 0; while (1) { done = f(1); if (done) break; leadingZeros++ }
  //   if (leadingZeros >= 32) return (1 << 32) - 1
  //   value = f(leadingZeros); return value + (1 << leadingZeros) - 1
  // The AV1 loop has no cap: the zeros are consumed until the first one bit, so a long zero run costs time
  // linear in the buffer and ends at the buffer end with BR-0001. With 32 or more zeros the value bits are
  // not read (WI-1.103 pitfall).
  const uint64_t start = bit_pos();
  Cursor c = cur_;
  uint64_t leading_zeros = 0;
  for (;;) {
    if (pos_of(c) >= size_bits()) {
      throw_bounds(leading_zeros + 1, start, size_bits());
    }
    // Count a whole cache's worth of zeros at once; the loop stays bounded by the buffer size.
    if (c.cache_bits == 0) {
      refill(c);
    }
    const auto zeros = static_cast<unsigned>(std::countl_zero(c.cache));
    if (zeros >= c.cache_bits) {
      leading_zeros += c.cache_bits;
      (void)take(c, c.cache_bits);
      continue;
    }
    leading_zeros += zeros;
    (void)take(c, zeros + 1);  // the zeros and the terminating one bit
    break;
  }
  if (leading_zeros >= kUvlcMaxLeadingZeros) {
    cur_ = c;
    return UINT32_MAX;
  }
  const auto lz = static_cast<unsigned>(leading_zeros);
  if (lz > size_bits() - pos_of(c)) {
    throw_bounds((2 * leading_zeros) + 1, start, size_bits());
  }
  const uint64_t value = take(c, lz);
  cur_ = c;
  // lz <= 31: value + 2^lz - 1 <= 2^32 - 2.
  return static_cast<uint32_t>(value + (uint64_t{1} << lz) - 1);
}

int64_t BitReader::su(unsigned n) {
  // AV1 §4.10.6: value = f(n); signMask = 1 << (n-1); if (value & signMask) value -= 2*signMask.
  // That is two's complement of width n, which i(n) implements for n in [1,64] (BR-0003 otherwise).
  return i(n);
}

uint32_t BitReader::ns(uint32_t n) {
  // AV1 §4.10.7:
  //   w = FloorLog2(n) + 1; m = (1 << w) - n; v = f(w - 1); if (v < m) return v
  //   extra_bit = f(1); return (v << 1) - m + extra_bit
  const uint64_t start = bit_pos();
  if (n == 0) {
    throw_range("ns(n)", start);
  }
  const auto w = static_cast<unsigned>(std::bit_width(n));  // FloorLog2(n) + 1, in [1,32]
  const uint64_t m = (uint64_t{1} << w) - n;
  Cursor c = cur_;
  if (w - 1 > bits_left()) {
    throw_bounds(w - 1, start, size_bits());
  }
  const uint64_t v = take(c, w - 1);
  if (v < m) {
    cur_ = c;
    return static_cast<uint32_t>(v);
  }
  if (pos_of(c) >= size_bits()) {
    throw_bounds(w, start, size_bits());
  }
  const uint64_t extra_bit = take(c, 1);
  cur_ = c;
  // v >= m, and the result is < n, so it fits.
  return static_cast<uint32_t>((v << 1U) - m + extra_bit);
}

uint64_t BitReader::le(unsigned n) {
  // AV1 §4.10.4: t = 0; for (i = 0; i < n; i++) { byte = f(8); t += (byte << (i * 8)) }
  if (n == 0 || n > kLeMaxBytes) {
    throw_width(n, bit_pos());
  }
  require_aligned();
  require_bits(uint64_t{n} * 8);
  uint64_t t = 0;
  for (unsigned idx = 0; idx < n; ++idx) {
    t |= take(cur_, 8) << (idx * 8);
  }
  return t;
}

int32_t BitReader::delta_q() {
  // AV1 §5.9.13 read_delta_q(): delta_coded = f(1); return delta_coded ? su(1+6) : 0.
  require_bits(1);
  if (!peek_bit()) {
    (void)take(cur_, 1);
    return 0;
  }
  require_bits(1 + kDeltaQBits);  // both reads checked up front, so a short buffer leaves bit_pos() alone
  (void)take(cur_, 1);
  return static_cast<int32_t>(su(kDeltaQBits));
}

// Non-const as declared in §7.2.2, like every other descriptor; the header is not changed by this WI.
uint64_t BitReader::L(unsigned /*n*/) {  // NOLINT(readability-make-member-function-const)
  throw ParseError("BR-0010", "L(n) requires the AV1 symbol decoder", bit_pos());
}

void BitReader::trailing_bits(uint64_t nb_bits) {
  // AV1 §5.3.4: trailing_one_bit f(1) (== 1); nbBits--; while (nbBits > 0) { trailing_zero_bit f(1) (== 0); nbBits-- }
  const uint64_t start = bit_pos();
  if (nb_bits == 0) {
    // SPEC-ASSUMPTION(#85): nbBits == 0 means the caller's payload already used every bit the OBU size allows;
    // AV1 would still read trailing_one_bit past that size. Reported as an invalid range argument.
    throw_range("trailing_bits", start);
  }
  require_bits(nb_bits);
  Cursor c = cur_;
  if (take(c, 1) != 1) {
    throw_trailing("trailing_one_bit", start, 0);
  }
  uint64_t left = nb_bits - 1;
  while (left > 0) {
    const auto chunk = static_cast<unsigned>(left < kZeroChunk ? left : kZeroChunk);
    const uint64_t at = pos_of(c);
    const uint64_t bits = take(c, chunk);
    if (bits != 0) {
      // The first one bit in the chunk is the offending trailing_zero_bit.
      const unsigned offset = chunk - static_cast<unsigned>(std::bit_width(bits));
      throw_trailing("trailing_zero_bit", at + offset, 1);
    }
    left -= chunk;
  }
  cur_ = c;
}

void BitReader::byte_alignment() {
  // AV1 §5.3.5: while (get_position() & 7) zero_bit f(1) (== 0). The bits of the current byte always exist.
  Cursor c = cur_;
  while (pos_of(c) % 8 != 0) {
    const uint64_t at = pos_of(c);
    if (take(c, 1) != 0) {
      throw_trailing("zero_bit", at, 1);
    }
  }
  cur_ = c;
}

}  // namespace ovt
