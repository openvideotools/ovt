// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// parse_parameter_set(codec, bytes) → syntax tree: the entry point through which the container decoders hand the
// parameter sets embedded in a codec configuration record (`avcC`, `hvcC`, `av1C`; chapter 08 §8.3.3, WI-1.207) to
// the syntax module (chapter 07). Internal to libovt-core: not a public header.
//
// One call parses one H.264 / H.265 NAL unit (no start code, no length prefix, emulation prevention bytes still in
// place) or one AV1 OBU (header included). A ParameterSetContext carries the SPS / VPS already parsed from the same
// record, so a PPS or an SEI that follows sees its SPS instead of being parsed provisionally.

#ifndef OVT_SRC_SYNTAX_COMMON_PARAMETER_SET_H
#define OVT_SRC_SYNTAX_COMMON_PARAMETER_SET_H

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ovt_model_generated.h"
#include "syntax/common/syntax_node_buffer.h"

namespace ovt::syntax {

/// The parameter sets one configuration record has delivered so far (H.264 SPS, H.265 VPS / SPS).
class ParameterSetContext {
 public:
  ParameterSetContext();
  ~ParameterSetContext();
  ParameterSetContext(const ParameterSetContext&) = delete;
  ParameterSetContext& operator=(const ParameterSetContext&) = delete;
  ParameterSetContext(ParameterSetContext&&) noexcept;
  ParameterSetContext& operator=(ParameterSetContext&&) noexcept;

  struct Impl;  // parameter_set.cpp
  [[nodiscard]] Impl& impl() noexcept { return *impl_; }

 private:
  std::unique_ptr<Impl> impl_;
};

struct ParameterSetResult {
  SyntaxNodeBuffer tree;                // root "nal_unit" / "open_bitstream_unit"; spans are RBSP bits
  std::vector<uint32_t> epb_positions;  // raw byte offsets of the removed emulation prevention bytes (H.26x)
  uint32_t unit_type = 0;               // nal_unit_type or obu_type
  bool has_parser = false;              // the unit type is a parameter set / SEI / metadata the module parses
  bool complete = false;                // parsed to its end without a ParseError
};

/// H.264: SPS (7), PPS (8), SPS extension (13), subset SPS (15). H.265: VPS (32), SPS (33), PPS (34), SEI (39, 40).
/// AV1: sequence header (1) and metadata (5) OBUs. Any other unit gets its header parsed only (`has_parser` false).
/// Never throws: a truncated or malformed unit keeps the nodes read before the error and `complete` is false; the
/// ParseError becomes a diagnostic on the root. `codec` other than H264, H265 and AV1 gives an empty tree.
[[nodiscard]] ParameterSetResult parse_parameter_set(fb::Codec codec, std::span<const uint8_t> bytes,
                                                     ParameterSetContext* ctx = nullptr);

}  // namespace ovt::syntax

#endif  // OVT_SRC_SYNTAX_COMMON_PARAMETER_SET_H
