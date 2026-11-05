// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of ovt::BitReader (chapter 14 §14.7.1, WI-0.557). Input layout: byte 0 = tape length T, the next
// min(T, size-1) bytes are the opcode tape, the rest is the payload the reader reads. A tape byte is
// `opcode | param << 5` (opcode = low 5 bits, wrapped modulo the opcode count; param = high 3 bits, an index
// into the width tables). After every operation the position invariants are checked, and a read that throws
// ParseError must leave the cursor where it was (bitreader.hpp). A violated invariant aborts; any other
// exception escapes and is a finding too.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"

namespace {

constexpr std::array<unsigned, 8> kWidth = {0, 1, 7, 8, 16, 32, 57, 64};
constexpr std::array<uint32_t, 8> kLimit = {0, 1, 2, 3, 5, 255, 65536, UINT32_MAX};
constexpr unsigned kOpcodes = 27;
volatile uint8_t g_touch = 0;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

struct Sink final : ovt::BitReader::DiagnosticSink {
  void reader_warning(std::string_view /*code*/, std::string_view /*message*/, uint64_t /*bit_offset*/) override {}
};

void run_op(ovt::BitReader& br, unsigned op, unsigned param) {
  const unsigned n = kWidth[param];
  const uint32_t lim = kLimit[param];
  switch (op) {
    case 0:
      require(n == 64 || br.u(n) >> n == 0);
      break;
    case 1:
      (void)br.i(std::max(n, 1U));
      break;
    case 2:
      require(br.ue() <= UINT32_MAX - 1);
      break;
    case 3:
      (void)br.se();
      break;
    case 4:
      (void)br.te(lim);
      break;
    case 5:
      require(br.leb128() < (uint64_t{1} << 56));
      break;
    case 6:
      (void)br.uvlc();
      break;
    case 7:
      (void)br.su(std::max(n, 1U));
      break;
    case 8:
      require(br.ns(lim) < std::max(lim, 1U));
      break;
    case 9:
      (void)br.le(param + 1);
      break;
    case 10:
      br.skip_bits(uint64_t{param} * param * 3);
      break;
    case 11:
      br.skip_bytes(param);
      break;
    case 12:
      br.skip_bits((8 - br.bit_pos() % 8) % 8);
      break;  // byte_align()
    case 13:
      (void)br.more_rbsp_data();
      break;
    case 14:
      require(n == 64 || br.peek_bits(n) >> n == 0);
      break;
    case 15:
      br.seek_bits(br.bit_pos() / 2 + (uint64_t{param} << 3));
      break;
    case 16:
      (void)br.delta_q();
      break;
    case 17:
      (void)br.b8();
      break;
    case 18:
      (void)br.st();
      break;
    case 19:
      br.rbsp_trailing_bits();
      break;
    case 20:
      br.trailing_bits(lim);
      break;
    case 21:
      br.byte_alignment();
      break;
    case 22: {
      const auto m = br.mark();
      br.skip_bits(n);
      require(br.span_since(m).bit_offset == m);
      break;
    }
    case 23:
      require(br.remaining_bytes().size() * 8 <= br.bits_left());
      break;
    case 24: {
      const auto s = br.bytes(br.bit_pos() / 8, param);
      require(s.size() == param);
      if (!s.empty()) {
        g_touch = g_touch ^ s.back();  // make ASan see the last byte
      }
      break;
    }
    case 25:
      (void)br.me(param & 3U, (param & 4U) != 0);
      break;
    default:
      br.expect_f(n, 0, "fuzz");
      break;
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const size_t tape_len = std::min<size_t>(data[0], size - 1);
  const uint8_t* tape = data + 1;
  const std::span<const uint8_t> payload(data + 1 + tape_len, size - 1 - tape_len);
  ovt::BitReader br(payload);
  Sink sink;
  br.set_diagnostic_sink(&sink);
  for (size_t k = 0; k < tape_len; ++k) {
    const uint64_t before = br.bit_pos();
    try {
      run_op(br, (tape[k] & 31U) % kOpcodes, tape[k] >> 5);
    } catch (const ovt::ParseError&) {
      require(br.bit_pos() == before);  // a failed read restores the cursor
    }
    require(br.bit_pos() + br.bits_left() == br.size_bits());
    require(br.size_bits() == payload.size() * 8);
  }
  return 0;
}
