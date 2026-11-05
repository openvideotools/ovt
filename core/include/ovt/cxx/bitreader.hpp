// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::BitReader: the mandatory bounds-checked bit reader of every parser (chapter 7 §7.2, chapter 0
// §0.7.1), and ovt::ParseError, the exception it throws. C++ helper header (D22): semi-public, no ABI
// promise, never included from a C header.
//
// The public declarations below are chapter 7 §7.2.2 verbatim, plus: [[nodiscard]] on the value-returning
// methods (clang-tidy modernize-use-nodiscard), `explicit` on the span constructor, and the private state.
// No signature changes.
//
// WI-1.102 implements the H.264/H.265 descriptors, the position API, the helpers and the diagnostic sink
// (core/src/bitreader/bitreader.cpp). The AV1 descriptors are declared here so that the header is settled
// once; WI-1.103 defines them in core/src/bitreader/bitreader_av1.cpp. Until then calling one is a link error.

#ifndef OVT_CXX_BITREADER_HPP
#define OVT_CXX_BITREADER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ovt {

struct ParseError : std::runtime_error {
  std::string code;     // stable code, e.g. "BR-0001" (table 7.2.4)
  uint64_t bit_offset;  // position at which the failing read started
  ParseError(std::string code, std::string message, uint64_t bit_offset);
};

struct BitSpan {
  uint64_t bit_offset;
  uint32_t bit_length;
};  // same layout as ovt.fb.BitSpan

class BitReader {
 public:
  using Mark = uint64_t;  // an absolute bit position

  explicit BitReader(std::span<const uint8_t> data) noexcept;  // data must outlive the reader
  BitReader(const uint8_t* data, size_t size) noexcept;

  // ---- position --------------------------------------------------------------------------
  [[nodiscard]] uint64_t bit_pos() const noexcept;    // bits consumed so far
  [[nodiscard]] uint64_t bits_left() const noexcept;  // size*8 - bit_pos()
  [[nodiscard]] uint64_t size_bits() const noexcept;
  [[nodiscard]] bool byte_aligned() const noexcept;  // bit_pos() % 8 == 0           (H.26x byte_aligned())
  void seek_bits(uint64_t abs_bit_pos);              // BR-0002 if > size_bits()
  void skip_bits(uint64_t n);                        // BR-0001 if n > bits_left()
  void skip_bytes(uint64_t n);                       // requires byte_aligned(), else BR-0004
  [[nodiscard]] Mark mark() const noexcept { return bit_pos(); }
  [[nodiscard]] BitSpan span_since(Mark m) const noexcept;         // {m, bit_pos()-m}; length saturates at UINT32_MAX
  [[nodiscard]] std::span<const uint8_t> remaining_bytes() const;  // requires byte_aligned(); view of unread bytes
  [[nodiscard]] std::span<const uint8_t> bytes(size_t byte_off,
                                               size_t n) const;  // bounds-checked raw access (BR-0001)

  // ---- peek (never advance) ------------------------------------------------------------------
  [[nodiscard]] uint64_t peek_bits(unsigned n) const;  // n <= 64; BR-0001 if n > bits_left()
  [[nodiscard]] bool peek_bit() const;

  // ---- H.264 / H.265 descriptors (§7.2 of either spec) -------------------------------------
  uint64_t u(unsigned n);      // u(n): unsigned, MSB first. n in [0,64]. n == 0 returns 0 and reads nothing.
  uint64_t f(unsigned n);      // f(n): same as u(n); the *caller* checks the fixed value (7.2.3 helper expect_f)
  uint8_t b8();                // b(8): one byte, any value
  int64_t i(unsigned n);       // i(n): two's complement, MSB first, n in [1,64]
  uint32_t ue();               // ue(v): Exp-Golomb order 0, §9.1. Result <= 2^32-2. BR-0005 if leadingZeroBits > 31.
  int32_t se();                // se(v): §9.1.1: k = ue(); value = (k & 1) ? (k+1)/2 : -(k/2)
  uint32_t te(uint32_t cmax);  // te(v): §9.1.2: cmax > 1 → ue(); cmax == 1 → !u(1); cmax == 0 → BR-0006
  // me(v): §9.1.3 maps codeNum=ue() to coded_block_pattern through Table 9-4; it appears only in
  // macroblock_layer() (slice data) which OVT never parses (ADR-008). Provided for completeness:
  uint32_t me(uint32_t chroma_array_type, bool is_intra);  // returns coded_block_pattern via Table 9-4 (both columns)
  std::string st();  // st(v): UTF-8 bytes up to and excluding a 0x00 byte; requires byte_aligned() (BR-0004);
                     // BR-0001 if no terminator before the end; max 65535 bytes (BR-0007)

  // helpers
  void expect_f(unsigned n, uint64_t value,
                const char* name);  // reads f(n); ParseError SYN-GEN-0003 "forbidden/reserved value" if != value
  [[nodiscard]] bool more_rbsp_data() const;  // H.264 §7.2 / H.265 §7.2 definition, algorithm 7.2.3
  void rbsp_trailing_bits();  // §7.3.2.11: rbsp_stop_one_bit (=1) then rbsp_alignment_zero_bit (=0) until
                              // byte_aligned(); BR-0008 on wrong values
  [[nodiscard]] bool more_data_in_byte_stream() const noexcept { return bits_left() > 0; }

  // ---- AV1 descriptors (§4.10) ---------------------------------------------------------------
  // f(n) is shared with the u(n) above (AV1 §4.10.2 == same bit order).
  uint64_t leb128();        // AV1 §4.10.5: up to 8 bytes; result < 2^56. If the 8th byte still has its continuation
                            // bit set (conformance violation, not a read error) the bit is ignored, BR-0009 is reported
                            // as a Warning to the attached DiagnosticSink (see "diagnostic sink" below; dropped when
                            // none is attached) and reading continues
  uint32_t uvlc();          // AV1 §4.10.3: leadingZeros counted until a 1; if leadingZeros >= 32 return (1<<32)-1; else
                            // f(leadingZeros) + (1<<leadingZeros) - 1
  int64_t su(unsigned n);   // AV1 §4.10.6: value = f(n); signMask = 1 << (n-1); if (value & signMask) value -=
                            // 2*signMask
  uint32_t ns(uint32_t n);  // AV1 §4.10.7: w = FloorLog2(n)+1; m = (1<<w)-n; v = f(w-1); if v < m return v;
                            // extra = f(1); return (v<<1) - m + extra.   n == 0 → BR-0006
  uint64_t le(unsigned n);  // AV1 §4.10.4: n bytes little-endian, requires byte_aligned() (BR-0004), n in [1,8]
  int32_t delta_q();        // AV1 §5.9.13 read_delta_q(): delta_coded = f(1); return delta_coded ? su(1+6) : 0
  // L(n) (AV1 §4.10.8) and S()/NS(n) are symbol-decoder (arithmetic) descriptors used only inside tile data,
  // which OVT does not parse. Declared so parsers can `static_assert` they are never called on a BitReader:
  [[noreturn]] uint64_t L(unsigned n);   // always throws BR-0010 "L(n) requires the AV1 symbol decoder"
  void trailing_bits(uint64_t nb_bits);  // AV1 §5.3.4: trailing_one_bit f(1) == 1, then nb_bits-1 zero bits == 0;
                                         // BR-0008 on mismatch
  void byte_alignment();                 // AV1 §5.3.5: zero bits until byte_aligned(); BR-0008 if a one bit is seen

  // ---- diagnostic sink (non-fatal reader findings, e.g. BR-0009) -------------------------------
  // Fatal problems are thrown as ParseError. Non-fatal conformance findings are reported to an attached sink.
  // SyntaxBuilder implements the sink and attaches itself in its constructor (§7.3.2), so a parser never calls
  // this.
  struct DiagnosticSink {
    virtual void reader_warning(std::string_view code, std::string_view message, uint64_t bit_offset) = 0;

   protected:
    ~DiagnosticSink() = default;
  };
  void set_diagnostic_sink(DiagnosticSink* sink) noexcept;         // non-owning; nullptr detaches; returns nothing
  [[nodiscard]] DiagnosticSink* diagnostic_sink() const noexcept;  // currently attached sink or nullptr

  // ---- diagnostics ---------------------------------------------------------------------------
  // Reads are counted; parsers call this at the end of a unit to flag unread trailing bytes
  // (Diagnostic Info "N bytes of trailing data", not an error — H.264 §7.4.1 allows cabac_zero_words etc.).
  [[nodiscard]] uint64_t unread_bytes_after_trailing() const noexcept;

 private:
  // The read position is kept as a 64-bit cache of not yet consumed bits (left-aligned, MSB = next bit),
  // refilled one byte at a time from data_[next_byte]: no unaligned loads of the input (§7.2.2).
  // bit_pos() == next_byte * 8 - cache_bits. A read that throws restores the whole cursor.
  struct Cursor {
    uint64_t cache = 0;
    unsigned cache_bits = 0;
    size_t next_byte = 0;
  };

  void refill(Cursor& c) const noexcept;
  uint64_t take_short(Cursor& c, unsigned n) const noexcept;  // n <= 57 and n <= bits left in c; no checks
  uint64_t take(Cursor& c, unsigned n) const noexcept;        // n <= 64 and n <= bits left in c; no checks
  void set_pos(Cursor& c, uint64_t abs_bit_pos) const noexcept;
  [[nodiscard]] static uint64_t pos_of(const Cursor& c) noexcept;
  void require_bits(uint64_t n) const;  // BR-0001 unless n <= bits_left()
  void require_aligned() const;         // BR-0004 unless byte_aligned()
  void warn(std::string_view code, std::string_view message, uint64_t bit_offset) const;

  std::span<const uint8_t> data_;
  Cursor cur_;
  uint64_t stop_bit_pos_;  // §7.2.3: position of rbsp_stop_one_bit, or kNoStopBit
  DiagnosticSink* sink_ = nullptr;
};

}  // namespace ovt

#endif  // OVT_CXX_BITREADER_HPP
