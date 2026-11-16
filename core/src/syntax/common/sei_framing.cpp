// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared SEI framing (chapter 7 §7.3.4 row 7, WI-1.110). See sei_framing.h.

#include "syntax/common/sei_framing.h"

#include <fmt/format.h>

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {

namespace {

constexpr std::string_view kOverrunCode = "BR-0001";

// payloadType / payloadSize: ff_byte f(8) = 0xFF repeated, then last_*_byte u(8) (§7.3.2.3.1).
uint64_t read_ff_coded(BitReader& br, SyntaxBuilder& b, std::string_view ff_name, std::string_view last_name) {
  uint64_t value = 0;
  while (br.peek_bits(8) == 0xFF) {
    (void)b.read_f(ff_name, 8, 0xFF);
    value += 255;
  }
  value += b.read_u(last_name, 8);
  return value;
}

}  // namespace

SeiMessageHeader read_sei_message_header(BitReader& br, SyntaxBuilder& b) {
  const uint64_t type = read_ff_coded(br, b, "ff_byte", "last_payload_type_byte");
  if (type > std::numeric_limits<uint32_t>::max()) {
    throw ParseError{std::string(kOverrunCode), fmt::format("payloadType {} does not fit 32 bits", type), br.bit_pos()};
  }
  b.derived("payloadType", type);
  const uint64_t size = read_ff_coded(br, b, "ff_byte", "last_payload_size_byte");
  b.derived("payloadSize", size);
  if (size > br.bits_left() / 8) {
    throw ParseError{std::string(kOverrunCode),
                     fmt::format("payloadSize {} exceeds the {} bytes left in the RBSP", size, br.bits_left() / 8),
                     br.bit_pos()};
  }
  return SeiMessageHeader{static_cast<uint32_t>(type), static_cast<uint32_t>(size)};
}

SeiPayloadReader::SeiPayloadReader(BitReader& br, SyntaxBuilder& b, uint32_t payload_size,
                                   std::string_view trailing_code)
    : br_(br),
      b_(b),
      trailing_code_(trailing_code),
      size_(payload_size),
      start_(br.bit_pos()),
      end_(br.bit_pos() + (uint64_t{payload_size} * 8)) {}

void SeiPayloadReader::require(uint64_t n) const {
  if (n > bits_left()) {
    throw ParseError{std::string(kOverrunCode), fmt::format("read of {} bits crosses the end of the SEI payload", n),
                     br_.bit_pos()};
  }
}

void SeiPayloadReader::check_end() const {
  if (br_.bit_pos() > end_) {
    throw ParseError{std::string(kOverrunCode), "variable-length element crosses the end of the SEI payload",
                     br_.bit_pos()};
  }
}

uint64_t SeiPayloadReader::u(std::string_view name, unsigned n) {
  require(n);
  return b_.read_u(name, n);
}

bool SeiPayloadReader::flag(std::string_view name) {
  require(1);
  return b_.read_flag(name);
}

int64_t SeiPayloadReader::i(std::string_view name, unsigned n) {
  require(n);
  return b_.read_i(name, n);
}

uint32_t SeiPayloadReader::ue(std::string_view name) {
  const uint32_t v = b_.read_ue(name);
  check_end();
  return v;
}

int32_t SeiPayloadReader::se(std::string_view name) {
  const int32_t v = b_.read_se(name);
  check_end();
  return v;
}

std::span<const uint8_t> SeiPayloadReader::bytes(std::string_view name, size_t n) {
  require(uint64_t{n} * 8);
  return b_.read_bytes(name, n);
}

void SeiPayloadReader::finish() {
  if (!br_.byte_aligned()) {
    (void)b_.read_f("payload_bit_equal_to_one", 1, 1);
    while (!br_.byte_aligned() && bits_left() > 0) {
      (void)b_.read_f("payload_bit_equal_to_zero", 1, 0);
    }
  }
  if (bits_left() > 0) {
    // The payload parser stopped before payloadSize: what is left is reserved_payload_extension_data.
    b_.diag(Severity::Info, trailing_code_, "SEI payload has trailing bytes after its last element");
    const size_t left_bytes = bits_left() / 8;
    (void)bytes("reserved_payload_extension_data", left_bytes);
  }
}

bool parse_sei_messages(BitReader& br, SyntaxBuilder& b, const SeiPayloadParser& parse_payload,
                        std::string_view message_spec_ref, std::string_view trailing_spec_ref,
                        std::string_view trailing_data_code) {
  int index = 0;
  do {
    b.begin_array_element("sei_message", index++, message_spec_ref);
    bool stop = false;
    try {
      const SeiMessageHeader header = read_sei_message_header(br, b);
      SeiPayloadReader payload(br, b, header.payload_size, trailing_data_code);
      try {
        parse_payload(header, payload);
        payload.finish();
      } catch (const ParseError& e) {
        // A malformed payload cannot overrun the next message: report it and continue after its last byte.
        b.diag(Severity::Error, e.code, e.what());
        br.seek_bits(payload.end_bit());
      }
    } catch (const ParseError& e) {
      b.diag(Severity::Error, e.code, e.what());
      stop = true;
    }
    b.end_array_element();
    if (stop) {
      return false;
    }
  } while (br.more_rbsp_data());
  auto s = b.scoped("rbsp_trailing_bits", trailing_spec_ref);
  (void)b.read_f("rbsp_stop_one_bit", 1, 1);
  while (!br.byte_aligned()) {
    (void)b.read_f("rbsp_alignment_zero_bit", 1, 0);
  }
  return true;
}

}  // namespace ovt::syntax
