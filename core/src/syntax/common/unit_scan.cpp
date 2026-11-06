// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit scanners (chapter 7 §7.3.3, WI-1.107). See ovt/cxx/syntax/unit_scan.hpp for the contract.
//
// Annex B: start codes are found with memchr(0x01) and a look-back for two zero bytes. Instead of re-reading a
// 4-byte overlap between windows, the scanner carries the length of the zero run that ends the previous window(s),
// so a start code (or a long zero run before it) that straddles a window boundary is seen exactly once. Positions
// inside the scanner are relative to the start of the scanned data; file offsets add the data's file offset.
// AV1: obu_header and every leb128 length are read through ovt::BitReader (chapter 0 §0.7.1).

#include "ovt/cxx/syntax/unit_scan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "syntax/common/unit_scan_internal.h"

namespace ovt::syntax {
namespace {

using Holder = io::RandomAccessFile::Holder;

constexpr std::string_view kCodeIoEof = "IO-0001";
constexpr std::string_view kCodeForbidden = "SYN-GEN-0004";  // SPEC-ASSUMPTION(#109)
constexpr std::string_view kCodeOverrun = "SYN-GEN-0105";
constexpr std::string_view kCodeObuNoSize = "SYN-AV1-0001";
constexpr std::string_view kMsgOverrun = "length-prefixed unit overruns its sample";
constexpr std::string_view kMsgObuNoSize = "obu_has_size_field = 0 in a position that requires a size";

void report(const DiagnosticCallback& diag, std::string_view code, std::string message, uint64_t file_offset) {
  if (diag) {
    diag(ScanDiagnostic{std::string(code), std::move(message), file_offset});
  }
}

// Number of zero bytes at the end of `s`. Compares 4 KiB blocks with memcmp first, so a long zero run (a sparse
// file) costs about as much as a memchr over it, even in a debug build.
size_t count_trailing_zeros(std::span<const uint8_t> s) {
  static constexpr std::array<uint8_t, 4096> kZeros{};
  const size_t n = s.size();
  size_t count = 0;
  while (n - count >= kZeros.size() &&
         std::memcmp(s.subspan(n - count - kZeros.size(), kZeros.size()).data(), kZeros.data(), kZeros.size()) == 0) {
    count += kZeros.size();
  }
  while (count < n && s[n - count - 1] == 0) {
    ++count;
  }
  return count;
}

// Index of the first byte equal to `value` in s[from, end), or s.size().
size_t find_byte(std::span<const uint8_t> s, size_t from, uint8_t value) {
  if (from >= s.size()) {
    return s.size();
  }
  const std::span<const uint8_t> rest = s.subspan(from);
  const void* hit = std::memchr(rest.data(), value, rest.size());
  if (hit == nullptr) {
    return s.size();
  }
  return from + static_cast<size_t>(static_cast<const uint8_t*>(hit) - rest.data());
}

// Offset (within `unit`) of the first 00 00 00 or 00 00 02, or unit.size() when there is none. H.264 §7.4.1 /
// H.265 §7.4.2 forbid both at a byte-aligned position inside a NAL unit (00 00 01 cannot occur: it is a boundary).
size_t find_forbidden_pattern(std::span<const uint8_t> unit) {
  size_t i = 0;
  while (true) {
    const size_t z = find_byte(unit, i, 0);
    if (z + 2 >= unit.size()) {
      return unit.size();
    }
    if (unit[z + 1] == 0 && (unit[z + 2] == 0 || unit[z + 2] == 2)) {
      return z;
    }
    i = z + 1;
  }
}

// ---- Annex B (H.264 / H.265) ---------------------------------------------------------------------------------

class AnnexBScanner {
 public:
  using Fetch = std::function<Holder(uint64_t pos, uint64_t len)>;  // positions relative to the scanned data

  AnnexBScanner(uint64_t data_file_offset, int64_t sample_index, const UnitCallback& cb, const DiagnosticCallback& diag,
                Fetch fetch)
      : base_(data_file_offset), sample_(sample_index), cb_(cb), diag_(diag), fetch_(std::move(fetch)) {}

  // Feeds the next window; `pos` is its position, consecutive windows are contiguous.
  void feed(uint64_t pos, std::span<const uint8_t> w) {
    size_t i = 0;
    while (true) {
      const size_t k = find_byte(w, i, 1);
      if (k == w.size()) {
        break;
      }
      size_t zeros = count_trailing_zeros(w.first(k));
      if (zeros == k) {
        zeros += carried_zeros_;  // the run continues into the previous window(s)
      }
      if (zeros >= 2) {
        on_start_code(pos + k, zeros);
      }
      i = k + 1;
    }
    const size_t tail = count_trailing_zeros(w);
    carried_zeros_ = (tail == w.size()) ? carried_zeros_ + tail : tail;
  }

  // End of data at `end`: the last NAL unit ends at its last non-zero byte (trailing_zero_8bits stripped).
  void finish(uint64_t end) {
    if (have_unit_) {
      emit(unit_start_, end - carried_zeros_);
      have_unit_ = false;
    }
  }

 private:
  // A start code whose 0x01 byte is at `one_pos`, preceded by `zeros` (>= 2) zero bytes. The byte before
  // unit_start_ is the previous 0x01, so the zero run never reaches into the previous NAL unit's start code.
  void on_start_code(uint64_t one_pos, uint64_t zeros) {
    const uint64_t zero_start = one_pos - zeros;
    if (have_unit_) {
      emit(unit_start_, zero_start);
    }
    prefix_ = ByteRange{zero_start, one_pos + 1 - zero_start};
    unit_start_ = one_pos + 1;
    have_unit_ = true;
  }

  void emit(uint64_t start, uint64_t end) {
    if (end <= start) {
      return;  // empty NAL unit: two start codes in a row, or a start code at EOF
    }
    const Holder h = fetch_(start, end - start);
    const std::span<const uint8_t> bytes = h.bytes;
    if (bytes.size() < end - start) {
      report(diag_, kCodeIoEof,
             fmt::format("unexpected end of file at offset {} ({} bytes requested)", base_ + start + bytes.size(),
                         end - start - bytes.size()),
             base_ + start + bytes.size());
      if (bytes.empty()) {
        return;
      }
    }
    if (const size_t f = find_forbidden_pattern(bytes); f < bytes.size()) {
      report(diag_, kCodeForbidden, fmt::format("forbidden byte pattern 00 00 {:02X} inside NAL unit", bytes[f + 2]),
             base_ + start + f);
    }
    cb_(ScannedUnit{base_ + start, bytes, ByteRange{base_ + prefix_.offset, prefix_.length}, sample_});
  }

  uint64_t base_;
  int64_t sample_;
  const UnitCallback& cb_;
  const DiagnosticCallback& diag_;
  Fetch fetch_;
  uint64_t carried_zeros_ = 0;  // zero bytes at the end of everything fed so far
  bool have_unit_ = false;
  uint64_t unit_start_ = 0;
  ByteRange prefix_;  // prefix of the unit that starts at unit_start_ (relative positions)
};

// ---- AV1 -----------------------------------------------------------------------------------------------------

// Forwards BitReader warnings (BR-0009 from leb128) to the diagnostic callback.
class ReaderWarnings final : public BitReader::DiagnosticSink {
 public:
  ReaderWarnings(const DiagnosticCallback& diag, uint64_t reader_file_offset)
      : diag_(diag), base_(reader_file_offset) {}
  void reader_warning(std::string_view code, std::string_view message, uint64_t bit_offset) override {
    report(diag_, code, std::string(message), base_ + bit_offset / 8);
  }

 private:
  const DiagnosticCallback& diag_;
  uint64_t base_;
};

void report_parse_error(const DiagnosticCallback& diag, const ParseError& e, uint64_t reader_file_offset) {
  report(diag, e.code, e.what(), reader_file_offset + e.bit_offset / 8);
}

}  // namespace

namespace detail {

void scan_annexb_windowed(const io::RandomAccessFile& file, size_t window_bytes, const UnitCallback& cb,
                          const DiagnosticCallback& diag) {
  if (window_bytes == 0) {
    throw std::invalid_argument("scan_annexb_windowed: window_bytes must be > 0");
  }
  const uint64_t size = file.size();
  file.advise(0, size, io::RandomAccessFile::Access::Sequential);
  AnnexBScanner scanner(0, kNoContainerSample, cb, diag,
                        [&file](uint64_t pos, uint64_t len) { return file.view(pos, len); });
  uint64_t pos = 0;
  while (pos < size) {
    const size_t len = std::min<uint64_t>(window_bytes, size - pos);
    const Holder h = file.view(pos, len);
    scanner.feed(pos, h.bytes);
    if (h.bytes.size() < len) {  // the file shrank under us
      report(diag, kCodeIoEof,
             fmt::format("unexpected end of file at offset {} ({} bytes requested)", pos + h.bytes.size(),
                         len - h.bytes.size()),
             pos + h.bytes.size());
      pos += h.bytes.size();
      break;
    }
    pos += len;
  }
  scanner.finish(pos);
}

}  // namespace detail

void scan_annexb(const io::RandomAccessFile& file, const UnitCallback& cb, const DiagnosticCallback& diag) {
  detail::scan_annexb_windowed(file, detail::kAnnexBWindowBytes, cb, diag);
}

void scan_annexb(std::span<const uint8_t> data, uint64_t data_file_offset, int64_t container_sample_index,
                 const UnitCallback& cb, const DiagnosticCallback& diag) {
  AnnexBScanner scanner(data_file_offset, container_sample_index, cb, diag,
                        [data](uint64_t pos, uint64_t len) { return Holder{nullptr, data.subspan(pos, len)}; });
  scanner.feed(0, data);
  scanner.finish(data.size());
}

void split_length_prefixed(std::span<const uint8_t> sample, unsigned nal_length_size, uint64_t sample_file_offset,
                           int64_t container_sample_index, const UnitCallback& cb, const DiagnosticCallback& diag) {
  if (nal_length_size != 1 && nal_length_size != 2 && nal_length_size != 4) {
    throw std::invalid_argument(
        fmt::format("split_length_prefixed: nal_length_size {} is not 1, 2 or 4", nal_length_size));
  }
  const size_t n = sample.size();
  size_t pos = 0;
  while (pos < n) {
    const uint64_t field_offset = sample_file_offset + pos;
    if (n - pos < nal_length_size) {  // length field cut off by the sample end
      report(diag, kCodeOverrun, std::string(kMsgOverrun), field_offset);
      return;
    }
    uint64_t length = 0;
    for (unsigned b = 0; b < nal_length_size; ++b) {
      length = (length << 8U) | sample[pos + b];
    }
    pos += nal_length_size;
    size_t end = n;
    if (length > n - pos) {
      report(diag, kCodeOverrun, std::string(kMsgOverrun), field_offset);
    } else {
      end = pos + length;
    }
    if (end > pos) {
      cb(ScannedUnit{sample_file_offset + pos, sample.subspan(pos, end - pos), ByteRange{field_offset, nal_length_size},
                     container_sample_index});
    }
    pos = end;
  }
}

void scan_obu_low_overhead(std::span<const uint8_t> data, uint64_t data_file_offset, int64_t container_sample_index,
                           ObuSpanKind kind, const UnitCallback& cb, const DiagnosticCallback& diag) {
  const size_t n = data.size();
  size_t pos = 0;
  while (pos < n) {
    const uint64_t obu_offset = data_file_offset + pos;
    BitReader br(data.subspan(pos));
    ReaderWarnings warnings(diag, obu_offset);
    br.set_diagnostic_sink(&warnings);
    size_t end = n;
    try {
      // obu_header(): forbidden_bit f(1), obu_type f(4), obu_extension_flag f(1), obu_has_size_field f(1),
      // obu_reserved_1bit f(1), then temporal_id f(3), spatial_id f(2), extension_header_reserved_3bits f(3).
      // The values are the OBU parser's business (§7.3.6 row 1); only the two flags matter here.
      br.skip_bits(5);
      const bool extension = br.u(1) != 0;
      const bool has_size = br.u(1) != 0;
      br.skip_bits(1);
      if (extension) {
        br.skip_bits(8);
      }
      if (has_size) {
        const uint64_t obu_size = br.leb128();
        const size_t header_bytes = br.bit_pos() / 8;
        if (obu_size > n - pos - header_bytes) {
          report(diag, kCodeOverrun, std::string(kMsgOverrun), obu_offset);
        } else {
          end = pos + header_bytes + obu_size;
        }
      } else if (kind == ObuSpanKind::Section5Stream) {
        report(diag, kCodeObuNoSize, std::string(kMsgObuNoSize), obu_offset);
      }
    } catch (const ParseError& e) {
      report_parse_error(diag, e, obu_offset);
      return;
    }
    cb(ScannedUnit{obu_offset, data.subspan(pos, end - pos), ByteRange{obu_offset, 0}, container_sample_index});
    pos = end;
  }
}

void scan_obu_annexb(std::span<const uint8_t> data, uint64_t data_file_offset, int64_t container_sample_index,
                     const UnitCallback& cb, const DiagnosticCallback& diag) {
  size_t pos = 0;
  size_t prefix_start = 0;  // the length fields since the last reported OBU form the next OBU's prefix_span

  // Reads a leb128 length at pos inside data[pos, limit) and returns the end of the unit it delimits, clamped
  // to limit (SYN-GEN-0105). Returns false when the field itself is cut off (reported with its BR code).
  const auto read_unit = [&](size_t limit, size_t& unit_end) -> bool {
    const uint64_t field_offset = data_file_offset + pos;
    BitReader br(data.subspan(pos, limit - pos));
    ReaderWarnings warnings(diag, field_offset);
    br.set_diagnostic_sink(&warnings);
    uint64_t length = 0;
    try {
      length = br.leb128();
    } catch (const ParseError& e) {
      report_parse_error(diag, e, field_offset);
      return false;
    }
    pos += br.bit_pos() / 8;
    if (length > limit - pos) {
      report(diag, kCodeOverrun, std::string(kMsgOverrun), field_offset);
      unit_end = limit;
    } else {
      unit_end = pos + length;
    }
    return true;
  };

  const size_t n = data.size();
  while (pos < n) {
    size_t tu_end = 0;
    if (!read_unit(n, tu_end)) {
      return;
    }
    while (pos < tu_end) {
      size_t fu_end = 0;
      if (!read_unit(tu_end, fu_end)) {
        return;
      }
      while (pos < fu_end) {
        size_t obu_end = 0;
        if (!read_unit(fu_end, obu_end)) {
          return;
        }
        if (obu_end > pos) {
          cb(ScannedUnit{data_file_offset + pos, data.subspan(pos, obu_end - pos),
                         ByteRange{data_file_offset + prefix_start, pos - prefix_start}, container_sample_index});
          prefix_start = obu_end;
        }
        pos = obu_end;
      }
    }
  }
}

}  // namespace ovt::syntax
