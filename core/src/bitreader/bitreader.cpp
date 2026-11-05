// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::BitReader: H.264/H.265 descriptors, position API, more_rbsp_data, rbsp_trailing_bits and the
// diagnostic sink (chapter 7 §7.2.2–§7.2.4, WI-1.102). The AV1 descriptors live in bitreader_av1.cpp
// (WI-1.103).
//
// Every public read checks its width and bounds before it touches the cursor, and every method that throws
// leaves bit_pos() where it was. Input bytes are reached only through std::span indexing (§0.7.1: no pointer
// arithmetic on input data).

#include "ovt/cxx/bitreader.hpp"

#include <array>
#include <bit>
#include <limits>
#include <string>
#include <utility>

namespace ovt {

// Byte indices and bit positions mix freely below; every supported platform is 64-bit (chapter 13).
static_assert(sizeof(size_t) == sizeof(uint64_t), "BitReader assumes a 64-bit size_t");

namespace {

constexpr uint64_t kNoStopBit = std::numeric_limits<uint64_t>::max();
constexpr unsigned kMaxWidth = 64;
// A read of up to this many bits is served from one refilled cache; wider reads take two steps (§7.2.2 step 2).
constexpr unsigned kSingleStepWidth = 57;
constexpr unsigned kMaxUeLeadingZeros = 31;
constexpr size_t kMaxStringBytes = 65535;

// H.264 Table 9-4: codeNum → coded_block_pattern. Column (a) for ChromaArrayType 1 or 2, column (b) for
// ChromaArrayType 0 or 3; each with the Intra_4x4/Intra_8x8 and the Inter prediction mode.
constexpr std::array<uint8_t, 48> kCbpIntraChroma = {47, 31, 15, 0,  23, 27, 29, 30, 7,  11, 13, 14, 39, 43, 45, 46,
                                                     16, 3,  5,  10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1,  2,  4,
                                                     8,  17, 18, 20, 24, 6,  9,  22, 25, 32, 33, 34, 36, 40, 38, 41};
constexpr std::array<uint8_t, 48> kCbpInterChroma = {0,  16, 1,  2,  4,  8,  32, 3,  5,  10, 12, 15, 47, 7,  11, 13,
                                                     14, 6,  9,  31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
                                                     17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41};
constexpr std::array<uint8_t, 16> kCbpIntraMono = {15, 0, 7, 11, 13, 14, 3, 5, 10, 12, 1, 2, 4, 8, 6, 9};
constexpr std::array<uint8_t, 16> kCbpInterMono = {0, 1, 2, 4, 8, 3, 5, 10, 12, 15, 7, 11, 13, 14, 6, 9};

std::string str(uint64_t v) {
  return std::to_string(v);
}

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

// The by-value message is the §7.2.2 signature; std::runtime_error copies it either way.
ParseError::ParseError(std::string code_, std::string message,  // NOLINT(performance-unnecessary-value-param)
                       uint64_t bit_offset_)
    : std::runtime_error(message), code(std::move(code_)), bit_offset(bit_offset_) {}

BitReader::BitReader(std::span<const uint8_t> data) noexcept : data_(data), stop_bit_pos_(kNoStopBit) {
  // §7.2.3 steps 2–3, once: the last non-zero byte L holds rbsp_stop_one_bit in its lowest set bit.
  for (size_t idx = data_.size(); idx > 0; --idx) {
    const uint8_t byte = data_[idx - 1];
    if (byte != 0) {
      stop_bit_pos_ = ((idx - 1) * 8) + (7U - static_cast<unsigned>(std::countr_zero(byte)));
      break;
    }
  }
  refill(cur_);
}

BitReader::BitReader(const uint8_t* data, size_t size) noexcept
    : BitReader(size == 0 ? std::span<const uint8_t>{} : std::span<const uint8_t>(data, size)) {}

// ---- cursor ----------------------------------------------------------------------------------------

void BitReader::refill(Cursor& c) const noexcept {
  while (c.cache_bits <= kMaxWidth - 8 && c.next_byte < data_.size()) {
    c.cache |= static_cast<uint64_t>(data_[c.next_byte]) << (kMaxWidth - 8 - c.cache_bits);
    c.cache_bits += 8;
    ++c.next_byte;
  }
}

uint64_t BitReader::take_short(Cursor& c, unsigned n) const noexcept {
  if (n == 0) {
    return 0;
  }
  if (c.cache_bits < n) {
    refill(c);
  }
  const uint64_t v = c.cache >> (kMaxWidth - n);
  c.cache <<= n;
  c.cache_bits -= n;
  return v;
}

uint64_t BitReader::take(Cursor& c, unsigned n) const noexcept {
  if (n <= kSingleStepWidth) {
    return take_short(c, n);
  }
  // Two steps (§7.2.2 step 2): a 57..64-bit read may not fit in one refilled cache.
  const uint64_t hi = take_short(c, n - 32);
  const uint64_t lo = take_short(c, 32);
  return (hi << 32U) | lo;
}

void BitReader::set_pos(Cursor& c, uint64_t abs_bit_pos) const noexcept {
  c = Cursor{};
  c.next_byte = abs_bit_pos / 8;
  refill(c);
  (void)take(c, static_cast<unsigned>(abs_bit_pos % 8));
}

uint64_t BitReader::pos_of(const Cursor& c) noexcept {
  return (c.next_byte * 8) - c.cache_bits;
}

void BitReader::require_bits(uint64_t n) const {
  if (n > bits_left()) {
    throw_bounds(n, bit_pos(), size_bits());
  }
}

void BitReader::require_aligned() const {
  if (!byte_aligned()) {
    throw ParseError("BR-0004", "operation requires byte alignment at bit " + str(bit_pos()), bit_pos());
  }
}

void BitReader::warn(std::string_view code, std::string_view message, uint64_t bit_offset) const {
  if (sink_ != nullptr) {
    sink_->reader_warning(code, message, bit_offset);
  }
}

// ---- position --------------------------------------------------------------------------------------

uint64_t BitReader::bit_pos() const noexcept {
  return pos_of(cur_);
}

uint64_t BitReader::bits_left() const noexcept {
  return size_bits() - bit_pos();
}

uint64_t BitReader::size_bits() const noexcept {
  return data_.size() * 8;
}

bool BitReader::byte_aligned() const noexcept {
  return bit_pos() % 8 == 0;
}

void BitReader::seek_bits(uint64_t abs_bit_pos) {
  if (abs_bit_pos > size_bits()) {
    throw ParseError("BR-0002", "seek to bit " + str(abs_bit_pos) + " beyond end", bit_pos());
  }
  set_pos(cur_, abs_bit_pos);
}

void BitReader::skip_bits(uint64_t n) {
  require_bits(n);
  set_pos(cur_, bit_pos() + n);
}

void BitReader::skip_bytes(uint64_t n) {
  require_aligned();
  if (n > bits_left() / 8) {
    throw_bounds(n > std::numeric_limits<uint64_t>::max() / 8 ? std::numeric_limits<uint64_t>::max() : n * 8, bit_pos(),
                 size_bits());
  }
  set_pos(cur_, bit_pos() + (n * 8));
}

BitSpan BitReader::span_since(Mark m) const noexcept {
  const uint64_t pos = bit_pos();
  const uint64_t len = pos > m ? pos - m : 0;
  constexpr uint64_t kMaxLen = std::numeric_limits<uint32_t>::max();
  return BitSpan{m, static_cast<uint32_t>(len > kMaxLen ? kMaxLen : len)};
}

std::span<const uint8_t> BitReader::remaining_bytes() const {
  require_aligned();
  return data_.subspan(bit_pos() / 8);
}

std::span<const uint8_t> BitReader::bytes(size_t byte_off, size_t n) const {
  if (byte_off > data_.size() || n > data_.size() - byte_off) {
    const uint64_t bits = n > std::numeric_limits<uint64_t>::max() / 8 ? std::numeric_limits<uint64_t>::max() : n * 8;
    throw_bounds(bits, byte_off * 8, size_bits());
  }
  return data_.subspan(byte_off, n);
}

// ---- peek ------------------------------------------------------------------------------------------

uint64_t BitReader::peek_bits(unsigned n) const {
  if (n > kMaxWidth) {
    throw_width(n, bit_pos());
  }
  require_bits(n);
  Cursor c = cur_;
  return take(c, n);
}

bool BitReader::peek_bit() const {
  return peek_bits(1) != 0;
}

// ---- H.264 / H.265 descriptors ---------------------------------------------------------------------

uint64_t BitReader::u(unsigned n) {
  if (n > kMaxWidth) {
    throw_width(n, bit_pos());
  }
  require_bits(n);
  return take(cur_, n);
}

uint64_t BitReader::f(unsigned n) {
  return u(n);
}

uint8_t BitReader::b8() {
  return static_cast<uint8_t>(u(8));
}

int64_t BitReader::i(unsigned n) {
  if (n == 0 || n > kMaxWidth) {
    throw_width(n, bit_pos());
  }
  const uint64_t v = u(n);
  // Sign-extend from bit n-1: conversion and arithmetic right shift are well defined since C++20.
  const unsigned shift = kMaxWidth - n;
  return static_cast<int64_t>(v << shift) >> shift;
}

uint32_t BitReader::ue() {
  const uint64_t start = bit_pos();
  Cursor c = cur_;
  unsigned leading_zeros = 0;
  // H.264 §9.1: count leading zero bits one at a time, capped so that a run of zeros cannot loop.
  for (;;) {
    if (pos_of(c) >= size_bits()) {
      throw_bounds(leading_zeros + 1, start, size_bits());
    }
    if (take(c, 1) != 0) {
      break;
    }
    if (++leading_zeros > kMaxUeLeadingZeros) {
      throw ParseError("BR-0005", "Exp-Golomb code too long at bit " + str(start), start);
    }
  }
  if (leading_zeros > size_bits() - pos_of(c)) {
    throw_bounds((2 * leading_zeros) + 1, start, size_bits());
  }
  const uint64_t suffix = take(c, leading_zeros);
  cur_ = c;
  // leading_zeros <= 31, so the result is at most 2^32 - 2.
  return static_cast<uint32_t>(((uint64_t{1} << leading_zeros) - 1) + suffix);
}

int32_t BitReader::se() {
  const int64_t k = ue();
  // H.264 §9.1.1 with a 64-bit intermediate: codeNum 2^32-2 maps to -(2^31-1) without overflow.
  return static_cast<int32_t>((k & 1) != 0 ? (k + 1) / 2 : -(k / 2));
}

uint32_t BitReader::te(uint32_t cmax) {
  if (cmax == 0) {
    throw_range("te(v)", bit_pos());
  }
  if (cmax == 1) {
    return u(1) == 0 ? 1U : 0U;  // one bit, inverted (H.264 §9.1.2)
  }
  return ue();
}

uint32_t BitReader::me(uint32_t chroma_array_type, bool is_intra) {
  const uint64_t start = bit_pos();
  if (chroma_array_type > 3) {
    throw_range("me(v)", start);
  }
  const Cursor saved = cur_;
  const uint32_t code_num = ue();
  const bool chroma = chroma_array_type == 1 || chroma_array_type == 2;
  const size_t limit = chroma ? kCbpIntraChroma.size() : kCbpIntraMono.size();
  if (code_num >= limit) {
    cur_ = saved;
    // SPEC-ASSUMPTION(#67): table 7.2.4 has no code for an out-of-range codeNum; BR-0011 is proposed.
    throw ParseError("BR-0011",
                     "coded_block_pattern codeNum " + str(code_num) + " out of range [0," + str(limit - 1) +
                         "] at bit " + str(start),
                     start);
  }
  if (chroma) {
    return is_intra ? kCbpIntraChroma.at(code_num) : kCbpInterChroma.at(code_num);
  }
  return is_intra ? kCbpIntraMono.at(code_num) : kCbpInterMono.at(code_num);
}

std::string BitReader::st() {
  require_aligned();
  const uint64_t start = bit_pos();
  const size_t first = start / 8;
  for (size_t idx = first; idx < data_.size(); ++idx) {
    if (data_[idx] == 0) {
      std::string out;
      out.reserve(idx - first);
      for (size_t k = first; k < idx; ++k) {
        out.push_back(static_cast<char>(data_[k]));
      }
      set_pos(cur_, (idx + 1) * 8);
      return out;
    }
    if (idx - first >= kMaxStringBytes) {
      throw ParseError("BR-0007", "string too long", start);
    }
  }
  throw_bounds((data_.size() - first + 1) * 8, start, size_bits());
}

// ---- helpers ---------------------------------------------------------------------------------------

void BitReader::expect_f(unsigned n, uint64_t value, const char* name) {
  const uint64_t start = bit_pos();
  const Cursor saved = cur_;
  const uint64_t got = f(n);
  if (got != value) {
    cur_ = saved;
    throw ParseError("SYN-GEN-0003", std::string(name) + ": forbidden/reserved value " + str(got), start);
  }
}

bool BitReader::more_rbsp_data() const {
  if (bits_left() == 0 || stop_bit_pos_ == kNoStopBit) {
    return false;
  }
  return bit_pos() < stop_bit_pos_;
}

void BitReader::rbsp_trailing_bits() {
  require_bits(1);
  Cursor c = cur_;
  const uint64_t stop_at = pos_of(c);
  if (take(c, 1) != 1) {
    throw_trailing("rbsp_stop_one_bit", stop_at, 0);
  }
  while (pos_of(c) % 8 != 0) {
    const uint64_t at = pos_of(c);
    if (take(c, 1) != 0) {
      throw_trailing("rbsp_alignment_zero_bit", at, 1);
    }
  }
  cur_ = c;
}

// ---- diagnostic sink -------------------------------------------------------------------------------

void BitReader::set_diagnostic_sink(DiagnosticSink* sink) noexcept {
  sink_ = sink;
}

BitReader::DiagnosticSink* BitReader::diagnostic_sink() const noexcept {
  return sink_;
}

// ---- diagnostics -----------------------------------------------------------------------------------

uint64_t BitReader::unread_bytes_after_trailing() const noexcept {
  // SPEC-ASSUMPTION(#68): the whole bytes after the byte holding the last consumed bit.
  return bits_left() / 8;
}

}  // namespace ovt
