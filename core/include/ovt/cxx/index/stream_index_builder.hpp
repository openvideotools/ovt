// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::index::StreamIndexBuilder: the single forward pass of chapter 7 §7.4.2 that turns the coded units of one
// video stream into the StreamIndex FlatBuffer of backbone §0.5.3 (WI-1.121). The caller owns the unit source
// (an Annex B / OBU scan, IVF frames, ISOBMFF samples split with §7.3.3) and hands the units over in stream
// order; the builder parses every header, groups units into frames, classifies the frames and derives the
// display order. C++ helper header (D22): semi-public, no ABI promise, never included from a C header.
//
// WI-1.121 is split per chapter 16 §16.2.6. Part a (this header, the codec-independent core, AV1 and the display
// order) leaves H.264 / H.265 dispatch to part b and the RAP map, source hash and C API to part c. The DPB
// models are stubs in every part: FrameInfo.declared_refs and dpb_after stay empty until WI-1.122, WI-1.123 and
// WI-1.124 fill them.

#ifndef OVT_CXX_INDEX_STREAM_INDEX_BUILDER_HPP
#define OVT_CXX_INDEX_STREAM_INDEX_BUILDER_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_model_generated.h"

namespace ovt::index {

/// Container timing of the sample a unit came from (§7.4.1). `timebase_num / timebase_den` seconds per tick.
struct SampleTiming {
  int64_t pts = 0;
  int64_t dts = 0;
  int32_t timebase_num = 1;
  int32_t timebase_den = 1;
};

/// One unit of the §7.4.1 input sequence: `(file_offset, unit_bytes, prefix_span, sample_index, pts, dts,
/// timebase)`. The first three are a ScannedUnit as the §7.3.3 scanners report it.
struct UnitInput {
  uint64_t file_offset = 0;                           // Unit.file_span.offset
  std::span<const uint8_t> bytes;                     // the unit, start code / length field excluded
  syntax::ByteRange prefix_span;                      // Unit.prefix_span
  int64_t sample_index = syntax::kNoContainerSample;  // Unit.sample_index
  const SampleTiming* timing = nullptr;               // null for an elementary stream (chapter 6 §6.2.6)
};

struct BuilderOptions {
  fb::Codec codec = fb::Codec::Unknown;
};

/// Thrown by the constructor for a codec the index has no dispatch for.
struct UnsupportedCodec : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class StreamIndexBuilder {
 public:
  /// @throws UnsupportedCodec unless `options.codec` is AV1 (H.264 and H.265 arrive with WI-1.121 part b).
  explicit StreamIndexBuilder(const BuilderOptions& options);
  ~StreamIndexBuilder();
  StreamIndexBuilder(const StreamIndexBuilder&) = delete;
  StreamIndexBuilder& operator=(const StreamIndexBuilder&) = delete;
  StreamIndexBuilder(StreamIndexBuilder&&) = delete;
  StreamIndexBuilder& operator=(StreamIndexBuilder&&) = delete;

  /// §7.4.2 S2–S7 for one unit. A ParseError of a header parser ends up as a Diagnostic of the unit and of the
  /// index; the call itself throws only std::bad_alloc / std::length_error. `u.bytes` is not kept.
  void add_unit(const UnitInput& u);

  /// A finding of the unit scanner that fed the builder (IO-0001, SYN-GEN-0105, SYN-AV1-0001, BR-*): becomes a
  /// stream-level Diagnostic with the registry severity of its code.
  void add_scan_diagnostic(const syntax::ScanDiagnostic& d);

  /// §7.4.2 S8–S10: closes the open frame, assigns the display order and the fallback time base, and returns
  /// the finished StreamIndex buffer (root table ovt.fb.StreamIndex, no file identifier). Deterministic
  /// (§0.7.5): the same units give the same bytes. Call once; add_unit() must not be called afterwards.
  [[nodiscard]] std::vector<uint8_t> finish(std::span<const uint8_t, 32> source_hash);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ovt::index

#endif  // OVT_CXX_INDEX_STREAM_INDEX_BUILDER_HPP
