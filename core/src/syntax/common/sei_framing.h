// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared SEI framing (chapter 7 §7.3.4 row 7, WI-1.110): sei_message() with its ff_byte loops, the payload size
// bound and the message loop of sei_rbsp(). H.264 uses it now; the H.265 SEI parsers (WI-1.114) reuse it.
//
// SPEC-ASSUMPTION(#528): §7.3.4 row 7 asks for "a sub-BitReader bounded to payloadSize bytes". SyntaxBuilder is
// bound to one BitReader and a second reader's bit offsets would restart at 0, so the payload is read through the
// message's own reader behind SeiPayloadReader, which checks every read against the payload end and throws the
// same ParseError BR-0001 a bounded reader would. Offsets in the tree stay absolute (equal to trace_headers').

#ifndef OVT_SRC_SYNTAX_COMMON_SEI_FRAMING_H
#define OVT_SRC_SYNTAX_COMMON_SEI_FRAMING_H

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {

/// payloadType and payloadSize of one sei_message() (§7.3.2.3.1).
struct SeiMessageHeader {
  uint32_t payload_type = 0;
  uint32_t payload_size = 0;
};

/// Reads the ff_byte / last_payload_type_byte / last_payload_size_byte elements and records the derived
/// `payloadType` and `payloadSize`. Throws ParseError BR-0001 when payloadSize exceeds the bytes left in the RBSP
/// (the framing then stops: the remaining messages are not parsed), and also when payloadType does not fit 32 bits.
SeiMessageHeader read_sei_message_header(BitReader& br, SyntaxBuilder& b);

/// Reads one payload through the message's reader and refuses to cross the payload end.
class SeiPayloadReader {
 public:
  /// `trailing_code` is the Info Diagnostic raised when payload bytes are left over after the payload parser.
  SeiPayloadReader(BitReader& br, SyntaxBuilder& b, uint32_t payload_size, std::string_view trailing_code);

  [[nodiscard]] BitReader& reader() noexcept { return br_; }
  [[nodiscard]] SyntaxBuilder& builder() noexcept { return b_; }
  [[nodiscard]] uint64_t start_bit() const noexcept { return start_; }
  [[nodiscard]] uint64_t end_bit() const noexcept { return end_; }
  [[nodiscard]] uint64_t bits_left() const noexcept { return br_.bit_pos() >= end_ ? 0 : end_ - br_.bit_pos(); }
  [[nodiscard]] uint32_t payload_size() const noexcept { return size_; }

  uint64_t u(std::string_view name, unsigned n);
  bool flag(std::string_view name);
  int64_t i(std::string_view name, unsigned n);
  uint32_t ue(std::string_view name);
  int32_t se(std::string_view name);
  /// n whole bytes (the reader must be byte aligned).
  std::span<const uint8_t> bytes(std::string_view name, size_t n);

  /// The payload-end rules of sei_payload(): payload_bit_equal_to_one / payload_bit_equal_to_zero up to the byte
  /// boundary, then the bytes that the payload parser did not consume as reserved_payload_extension_data.
  void finish();

 private:
  void require(uint64_t n) const;  // ParseError BR-0001 unless n bits remain in the payload
  void check_end() const;          // ParseError BR-0001 when a variable-length read crossed the payload end

  BitReader& br_;
  SyntaxBuilder& b_;
  std::string_view trailing_code_;
  uint32_t size_;
  uint64_t start_;
  uint64_t end_;
};

/// Called for each message with the reader positioned at the first payload byte.
using SeiPayloadParser = std::function<void(const SeiMessageHeader&, SeiPayloadReader&)>;

/// sei_rbsp() (§7.3.2.3): `do sei_message() while (more_rbsp_data())` and rbsp_trailing_bits(). Nodes
/// `sei_message[i]` are opened here; the parser adds one child node per payload. A malformed payload becomes an
/// Error Diagnostic (its code is the ParseError's) on the message and parsing continues after the payload; a
/// malformed header or a payloadSize beyond the RBSP becomes one Error Diagnostic and stops the framing.
/// Returns false when the framing stopped early.
bool parse_sei_messages(BitReader& br, SyntaxBuilder& b, const SeiPayloadParser& parse_payload,
                        std::string_view message_spec_ref, std::string_view trailing_spec_ref,
                        std::string_view trailing_data_code);

}  // namespace ovt::syntax

#endif  // OVT_SRC_SYNTAX_COMMON_SEI_FRAMING_H
