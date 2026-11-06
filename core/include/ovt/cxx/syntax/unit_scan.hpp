// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit scanners (chapter 7 §7.3.3, WI-1.107): they split a byte stream or a container sample into coded units
// (H.264/H.265 NAL units, AV1 OBUs) and report each unit's file span and prefix span (Unit.file_span /
// Unit.prefix_span of the StreamIndex, backbone §0.5.3). Shared by the `container` and `index` modules.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.
//
// §7.3.3 names the four functions and says that "each callback receives (file_offset_of_unit,
// unit_bytes_view, prefix_span, container_sample_index)". The signatures below make that concrete:
//  - the callback receives one ScannedUnit holding those four values;
//  - every span-based scanner also takes the file offset of span[0] and the container sample index, which the
//    callback needs and the span alone does not carry;
//  - split_length_prefixed() reports units through the same callback as the others;
//  - scan_annexb() also has a span overload for MPEG-TS PES payloads, which §7.3.3 lists as an input;
//  - scanner findings go to an optional DiagnosticCallback (§7.9 codes), because some of them belong to no unit
//    (a truncated length field, a header cut off by the end of the span).
// SPEC-ASSUMPTION(#109): the forbidden H.26x byte pattern of the WI-1.107 notes is reported as SYN-GEN-0004, and
// an AV1 length field (obu_size, temporal_unit_size, frame_unit_size, obu_length) that overruns its span as
// SYN-GEN-0105; §7.9 has no row for either.

#ifndef OVT_CXX_SYNTAX_UNIT_SCAN_HPP
#define OVT_CXX_SYNTAX_UNIT_SCAN_HPP

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace ovt::io {
class RandomAccessFile;
}

namespace ovt::syntax {

// A file byte range; same layout as ovt.fb.ByteSpan.
struct ByteRange {
  uint64_t offset = 0;
  uint64_t length = 0;
};

inline constexpr int64_t kNoContainerSample = -1;

// One unit, as a scanner hands it to the callback.
struct ScannedUnit {
  uint64_t file_offset = 0;                             // Unit.file_span.offset: file offset of bytes[0]
  std::span<const uint8_t> bytes;                       // the unit (start code / length field excluded); size() is
                                                        // Unit.file_span.length. Valid only during the callback.
  ByteRange prefix_span;                                // start code with its zero bytes, or the length field(s);
                                                        // length 0 (at file_offset) when the unit has none
  int64_t container_sample_index = kNoContainerSample;  // sample the unit came from, or kNoContainerSample
};

// A non-fatal scanner finding. Severity is the one of the code's §7.9 registry row: IO-0001, SYN-GEN-0105,
// SYN-AV1-0001 and the BR-* codes of a truncated header or length field are Error; BR-0009 and SYN-GEN-0004
// are Warning.
struct ScanDiagnostic {
  std::string code;      // e.g. "SYN-GEN-0105"
  std::string message;   // the registry message template, filled in
  uint64_t file_offset;  // where the finding is: the unit's first byte, or the field that failed
};

using UnitCallback = std::function<void(const ScannedUnit&)>;
using DiagnosticCallback = std::function<void(const ScanDiagnostic&)>;  // may be empty: findings are dropped

// ---- H.264 / H.265 Annex B byte stream ---------------------------------------------------------------------
// A start code is 00 00 01. The zero bytes before it (zero_byte, leading_zero_8bits, and the
// trailing_zero_8bits of the previous NAL unit) belong to the prefix_span of the unit that follows; a NAL unit
// ends at its last non-zero byte before the next start code or EOF. Bytes before the first start code and zero
// bytes after the last NAL unit belong to no unit. Empty NAL units (a start code directly followed by another
// one) are not reported. The pattern 00 00 00 or 00 00 02 inside a NAL unit is not a boundary: it is reported
// once per unit as SYN-GEN-0004.
//
// The file overload reads 1 MiB view() windows with Access::Sequential and finds a start code that straddles two
// windows exactly once. A NAL unit is handed over as one view(), so a NAL unit larger than RandomAccessFile can
// view (2 GiB) throws ovt::io::IoError. container_sample_index is kNoContainerSample.
void scan_annexb(const io::RandomAccessFile& file, const UnitCallback& cb, const DiagnosticCallback& diag = {});
// Span overload (MPEG-TS PES payloads): `data` starts at file offset `data_file_offset`.
void scan_annexb(std::span<const uint8_t> data, uint64_t data_file_offset, int64_t container_sample_index,
                 const UnitCallback& cb, const DiagnosticCallback& diag = {});

// ---- ISO/IEC 14496-15 length-prefixed sample -----------------------------------------------------------------
// Loop over `sample`: an N-byte big-endian length (N = nal_length_size ∈ {1, 2, 4}, from
// avcC/hvcC lengthSizeMinusOne + 1), then that many bytes. prefix_span is the length field. A length that
// overruns the sample is reported as SYN-GEN-0105 and the unit is truncated to the sample end; a length field cut
// off by the sample end is reported as SYN-GEN-0105 too and ends the sample. Zero lengths are skipped.
// Throws std::invalid_argument when nal_length_size is not 1, 2 or 4 (the caller validates avcC/hvcC).
void split_length_prefixed(std::span<const uint8_t> sample, unsigned nal_length_size, uint64_t sample_file_offset,
                           int64_t container_sample_index, const UnitCallback& cb, const DiagnosticCallback& diag = {});

// ---- AV1 low-overhead bitstream format (AV1 Section 5) --------------------------------------------------------
enum class ObuSpanKind : uint8_t {
  // An AV1 Annex B obu_length unit, an ISOBMFF av01 sample, an IVF frame or av1C.configOBUs: the last OBU may
  // omit obu_size.
  LengthDelimited,
  // A whole Section 5 stream (.obu file): every OBU must carry obu_size.
  Section5Stream,
};

// Parses obu_header() (1 byte, plus the extension byte when obu_extension_flag is set) and obu_size (leb128).
// Each unit is a whole OBU including header and obu_size; prefix_span is empty. An OBU without obu_size extends
// to the end of the span; in a Section5Stream that is SYN-AV1-0001. An obu_size that overruns the span is
// SYN-GEN-0105 (unit truncated to the span end); a header or obu_size cut off by the span end is reported with
// the BitReader's BR code and ends the scan.
void scan_obu_low_overhead(std::span<const uint8_t> data, uint64_t data_file_offset, int64_t container_sample_index,
                           ObuSpanKind kind, const UnitCallback& cb, const DiagnosticCallback& diag = {});

// ---- AV1 Annex B length-delimited bitstream format -----------------------------------------------------------
// temporal_unit(temporal_unit_size) { frame_unit(frame_unit_size) { obu_length, open_bitstream_unit } } (AV1
// Annex B.2). Each unit is one OBU (obu_length bytes, its own obu_size field included when present). The length
// fields before an OBU form its prefix_span, so the first OBU of a temporal unit carries temporal_unit_size,
// frame_unit_size and obu_length. A length that overruns its enclosing unit is SYN-GEN-0105 (truncated); a
// length field cut off by the end of its enclosing unit is reported with the BitReader's BR code and ends the
// scan. Zero-length OBUs are not reported; their obu_length joins the next unit's prefix_span.
void scan_obu_annexb(std::span<const uint8_t> data, uint64_t data_file_offset, int64_t container_sample_index,
                     const UnitCallback& cb, const DiagnosticCallback& diag = {});

}  // namespace ovt::syntax

#endif  // OVT_CXX_SYNTAX_UNIT_SCAN_HPP
