// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit splitter (chapter 08 §8.7.4, WI-1.231): turns one container sample (or demuxer packet) into the units the
// analysis plugins take through the ABI (backbone §0.6.2): NAL units without start code or length prefix, and AV1
// OBUs with obu_has_size_field = 1. Shared by the libavformat demuxer (Role B) and the own-reader path (Role A).
// Never adds start codes.

#ifndef OVT_DEMUX_UNIT_SPLIT_H
#define OVT_DEMUX_UNIT_SPLIT_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ovt_model_generated.h"

namespace ovt::demux {

// One unit inside a sample (§8.7.1). The prefix_length bytes before offset_in_packet are the length prefix or the
// start code; 0 for AV1 OBUs and raw frames.
struct UnitSpan {
  uint64_t offset_in_packet = 0;
  uint32_t length = 0;
  uint32_t prefix_length = 0;
  friend bool operator==(const UnitSpan&, const UnitSpan&) = default;
};

enum class Framing : uint8_t { LengthPrefixed, AnnexB, Av1Obu, Raw };

struct SplitResult {
  std::vector<UnitSpan> units;
  std::vector<ovt::fb::DiagnosticT> diagnostics;  // CNT-9001..9003, and the CNT-ESB-* findings of Annex B splitting
  bool rewritten = false;  // AV1: the last OBU had no size field; its Section 5 form is in `scratch`
};

// Splits `sample` per §8.7.4. `nal_length_size` (1, 2 or 4) is used by LengthPrefixed only. `scratch` is cleared and,
// when `rewritten` is set, receives the last OBU rewritten with obu_has_size_field = 1 and a leb128 obu_size (only
// that OBU, not the whole temporal unit). `temporal_delimiters_expected` is true for IVF and raw OBU temporal units,
// which start with an OBU_TEMPORAL_DELIMITER by definition; otherwise (ISOBMFF, Matroska) each delimiter raises
// CNT-9003.
// SPEC-ASSUMPTION(#338): `scratch` holds only the rewritten OBU, and `temporal_delimiters_expected` is an addition to
// the §8.7.4 signature (CNT-9003 applies to ISOBMFF samples only).
SplitResult split_units(std::span<const uint8_t> sample, Framing framing, uint8_t nal_length_size,
                        std::vector<uint8_t>& scratch, bool temporal_delimiters_expected = false);

// The i-th unit in plugin-ABI form: the sample bytes of units[i], or `scratch` for the rewritten last OBU. This is
// what Packet::unit_bytes (§8.7.1, WI-1.230) returns. Precondition: i < result.units.size().
[[nodiscard]] std::span<const uint8_t> unit_bytes(std::span<const uint8_t> sample, const SplitResult& result,
                                                  std::span<const uint8_t> scratch, std::size_t i) noexcept;

}  // namespace ovt::demux

#endif  // OVT_DEMUX_UNIT_SPLIT_H
