// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit splitter (chapter 08 §8.7.4, WI-1.231). Annex B splitting is the §8.6.3 splitter of WI-1.226 run over the
// sample bytes; leb128, obu_header and the size-field rewrite are the §8.6.4 helpers of WI-1.227.

#include "demux/unit_split.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "container/annexb.hpp"
#include "container/diagnostics.h"
#include "container/obu.hpp"
#include "container/split_types.hpp"

namespace ovt::demux {
namespace {

using container::DiagCode;

constexpr uint8_t kAnnexBInsideAvcLength = 4;  // 00 00 00 01 read as a 4-byte length prefix

class Splitter {
 public:
  Splitter(std::span<const uint8_t> sample, SplitResult& result) : sample_(sample), result_(result) {}

  void emit(uint64_t offset, uint64_t length, uint32_t prefix_length) {
    if (length == 0) {
      return;
    }
    // A sample is far below 4 GiB; the clamp only guards the narrowing.
    result_.units.push_back(
        UnitSpan{offset, static_cast<uint32_t>(std::min<uint64_t>(length, UINT32_MAX)), prefix_length});
  }

  // `severity` overrides the registry severity where §8.7.4 gives the code another one (zero-length NAL).
  void diagnose(DiagCode code, uint64_t offset, std::string detail,
                std::optional<container::Severity> severity = std::nullopt) {
    const auto& info = container::diag(code);
    fb::DiagnosticT d;
    d.severity = static_cast<fb::Severity>(severity.value_or(info.severity));
    d.code = std::string(info.code);
    d.message = std::move(detail) + " (sample offset " + std::to_string(offset) + ")";
    d.spec_ref = std::string(info.spec_ref);
    result_.diagnostics.push_back(std::move(d));
  }

  void length_prefixed(uint8_t nal_length_size);
  void annexb();
  void av1(std::vector<uint8_t>& scratch, bool temporal_delimiters_expected);

 private:
  std::span<const uint8_t> sample_;
  SplitResult& result_;
};

void Splitter::length_prefixed(uint8_t nal_length_size) {
  const uint64_t size = sample_.size();
  const uint64_t length_size = nal_length_size;
  if (nal_length_size != 1 && nal_length_size != 2 && nal_length_size != 4) {
    diagnose(DiagCode::CNT_9001, 0,
             "nal_length_size " + std::to_string(nal_length_size) + " is not 1, 2 or 4; the sample is one unit");
    emit(0, size, 0);
    return;
  }
  if (nal_length_size == kAnnexBInsideAvcLength && size >= 4 && sample_[0] == 0 && sample_[1] == 0 && sample_[2] == 0 &&
      sample_[3] == 1) {
    diagnose(DiagCode::CNT_9002, 0, "sample starts with 00 00 00 01; split as Annex B");
    annexb();
    return;
  }
  uint64_t pos = 0;
  while (pos + length_size <= size) {
    uint64_t n = 0;
    for (uint64_t k = 0; k < length_size; ++k) {
      n = (n << 8U) | sample_[pos + k];
    }
    if (n == 0) {
      diagnose(DiagCode::CNT_9001, pos, "zero-length NAL", container::Severity::Warning);
      pos += length_size;
      continue;
    }
    if (n > size - pos - length_size) {
      const uint64_t clamped = size - pos - length_size;
      diagnose(
          DiagCode::CNT_9001, pos,
          "NAL length " + std::to_string(n) + " exceeds sample; unit clamped to " + std::to_string(clamped) + " bytes");
      emit(pos + length_size, clamped, nal_length_size);
      return;
    }
    emit(pos + length_size, n, nal_length_size);
    pos += length_size + n;
  }
  if (pos < size) {
    diagnose(DiagCode::CNT_9001, pos,
             std::to_string(size - pos) + " trailing bytes shorter than the " + std::to_string(length_size) +
                 "-byte length field");
  }
}

void Splitter::annexb() {
  container::annexb_split(
      sample_, 0,
      [this](const container::NalSpan& nal) {
        emit(nal.file_span.offset, nal.file_span.length, static_cast<uint32_t>(nal.prefix_span.length));
      },
      [this](const container::Finding& f) {
        const auto& info = container::diag(f.code);
        fb::DiagnosticT d;
        d.severity = static_cast<fb::Severity>(info.severity);
        d.code = std::string(info.code);
        d.message = f.message;
        d.spec_ref = std::string(info.spec_ref);
        result_.diagnostics.push_back(std::move(d));
      });
}

void Splitter::av1(std::vector<uint8_t>& scratch, bool temporal_delimiters_expected) {
  const uint64_t size = sample_.size();
  uint64_t pos = 0;
  while (pos < size) {
    const auto rest = sample_.subspan(pos);
    const container::ObuHeader h = container::parse_obu_header(rest.first(std::min<std::size_t>(2, rest.size())));
    if (!h.valid || rest.size() < h.header_bytes) {
      diagnose(DiagCode::CNT_9001, pos, "malformed OBU header; the rest of the sample is one unit");
      emit(pos, rest.size(), 0);
      return;
    }
    if (h.obu_type == container::kObuTemporalDelimiter && !temporal_delimiters_expected) {
      diagnose(DiagCode::CNT_9003, pos, "OBU_TEMPORAL_DELIMITER inside the sample");
    }
    if (!h.has_size_field) {
      // AV1-ISOBMFF §2.4: only the last OBU may omit obu_size; it runs to the end of the sample.
      if (container::reframe_annexb_obu(rest, scratch)) {
        result_.rewritten = true;
      }
      emit(pos, rest.size(), 0);
      return;
    }
    const container::Leb128 obu_size = container::read_leb128(rest.subspan(h.header_bytes));
    if (obu_size.status != container::LebStatus::Ok) {
      diagnose(DiagCode::CNT_9001, pos, "malformed obu_size leb128; the rest of the sample is one unit");
      emit(pos, rest.size(), 0);
      return;
    }
    const uint64_t head = uint64_t{h.header_bytes} + obu_size.bytes;
    if (obu_size.value > rest.size() - head) {
      diagnose(DiagCode::CNT_9001, pos,
               "obu_size " + std::to_string(obu_size.value) + " exceeds sample; unit clamped to " +
                   std::to_string(rest.size()) + " bytes");
      emit(pos, rest.size(), 0);
      return;
    }
    emit(pos, head + obu_size.value, 0);
    pos += head + obu_size.value;
  }
}

}  // namespace

SplitResult split_units(std::span<const uint8_t> sample, Framing framing, uint8_t nal_length_size,
                        std::vector<uint8_t>& scratch, bool temporal_delimiters_expected) {
  SplitResult result;
  scratch.clear();
  Splitter splitter(sample, result);
  switch (framing) {
    case Framing::LengthPrefixed:
      splitter.length_prefixed(nal_length_size);
      break;
    case Framing::AnnexB:
      splitter.annexb();
      break;
    case Framing::Av1Obu:
      splitter.av1(scratch, temporal_delimiters_expected);
      break;
    case Framing::Raw:
      splitter.emit(0, sample.size(), 0);
      break;
  }
  return result;
}

std::span<const uint8_t> unit_bytes(std::span<const uint8_t> sample, const SplitResult& result,
                                    std::span<const uint8_t> scratch, std::size_t i) noexcept {
  if (result.rewritten && i + 1 == result.units.size()) {
    return scratch;
  }
  const UnitSpan& u = result.units[i];
  return sample.subspan(u.offset_in_packet, u.length);
}

}  // namespace ovt::demux
