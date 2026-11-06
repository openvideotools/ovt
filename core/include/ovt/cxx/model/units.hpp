// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Motion-vector unit conversion (chapter 06 §6.2.3) and the plane blob_id grammar (§6.6.2), WI-0.084.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_MODEL_UNITS_HPP
#define OVT_CXX_MODEL_UNITS_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ovt_model_generated.h"

namespace ovt::model {

// ---- Motion vectors (§6.2.3) ------------------------------------------------------------------------

/// Neutral MV unit: 1/16 luma sample. Converted values are clamped to ±kNeutralMvMax; -32768 is reserved for
/// the "MVD not available" sentinel and is never the result of a conversion.
inline constexpr std::int16_t kNeutralMvMax = 32767;
inline constexpr std::int16_t kMvdUnavailable = -32768;
inline constexpr int kNeutralUnitsPerLumaSample = 16;

/// Factor from the codec's MV unit to 1/16 luma sample: 4 for H.264 and HEVC (quarter sample), 2 for AV1
/// (eighth sample, intra block copy included). Throws std::invalid_argument for any other codec.
[[nodiscard]] int mv_factor(fb::Codec codec);

/// @p spec_mv × factor, or std::nullopt when the product lies outside ±32767 (an HEVC MV beyond ±2047.9375
/// luma samples; §6.2.3 makes the producer clamp and emit MDL-GEN-0003). Throws like mv_factor.
[[nodiscard]] std::optional<std::int16_t> mv_to_neutral_checked(std::int32_t spec_mv, fb::Codec codec);

/// @p spec_mv × factor, clamped to ±32767 (§6.2.3). Use mv_to_neutral_checked to learn whether it clamped.
/// Throws like mv_factor.
[[nodiscard]] std::int16_t mv_to_neutral(std::int32_t spec_mv, fb::Codec codec);

/// Inverse of mv_to_neutral: @p neutral / factor. The division must be exact; a remainder means the producer
/// stored unscaled values (e.g. AV1 1/8-pel values), and the function throws std::invalid_argument. It also
/// throws for kMvdUnavailable (-32768), which is a sentinel, not a vector.
// SPEC-ASSUMPTION(#98): WI-0.084 says "asserts divisibility"; this throws instead of assert() so that the
// check also holds in release builds and can be tested.
[[nodiscard]] std::int32_t mv_from_neutral(std::int16_t neutral, fb::Codec codec);

/// A neutral MV component in luma samples: @p neutral / 16.0.
[[nodiscard]] constexpr double mv_in_luma_samples(std::int16_t neutral) noexcept {
  return static_cast<double>(neutral) / kNeutralUnitsPerLumaSample;
}

// ---- blob_id (§6.6.2) -------------------------------------------------------------------------------

/// Which `stage` alternative of the §6.6.2 grammar a blob_id uses.
enum class BlobKind : std::uint8_t {
  Plane = 0,           ///< "recon", "pred", …, "coeff" (an ovt.fb.PlaneStage)
  MetricMap = 1,       ///< "map-<metric-id>" (ovt_metrics.fbs MapBlob)
  TransformBlock = 2,  ///< "tb-<Block.id>" (HevcTu / Av1Txb coeff_blob_id)
};

/// The parts of a blob_id. Fields not used by @ref kind keep their defaults.
// SPEC-ASSUMPTION(#98): WI-0.084 names BlobIdParts without defining it; this is the definition.
struct BlobIdParts {
  std::array<std::uint8_t, 32> source_hash{};  ///< StreamIndex.source_hash (§6.2.8)
  std::int32_t decode_index = 0;               ///< FrameId.decode_index, >= 0
  BlobKind kind = BlobKind::Plane;
  fb::PlaneStage stage = fb::PlaneStage::Reconstruction;  ///< kind == Plane
  std::string metric_id;                                  ///< kind == MetricMap: 1..32 of [A-Za-z0-9_]
  std::uint32_t tb_block_id = 0;                          ///< kind == TransformBlock
  std::uint8_t plane = 0;                                 ///< 0 y, 1 u, 2 v, 3 a
  friend bool operator==(const BlobIdParts&, const BlobIdParts&) = default;
};

/// The stage token of @p stage ("recon", "pred", "resid", "predeblock", "presaocdef", "prelr", "coeff"); empty
/// for a value outside the enum.
[[nodiscard]] std::string_view stage_token(fb::PlaneStage stage) noexcept;

/// `<64 lower-hex>/<decode_index>/<stage token>/<y|u|v|a>` (§6.6.2). Throws std::invalid_argument when
/// @p decode_index is negative, @p plane is not 0..3 or @p stage is outside the enum.
[[nodiscard]] std::string blob_id(std::span<const std::uint8_t, 32> hash, int decode_index, fb::PlaneStage stage,
                                  std::uint8_t plane);

/// Any blob_id, including "map-<metric-id>" and "tb-<id>" stages. Throws std::invalid_argument when a part
/// breaks the grammar (also an invalid metric_id).
[[nodiscard]] std::string blob_id(const BlobIdParts& parts);

/// Strict parser for the §6.6.2 ABNF, hand-written (no regex). Returns false, and leaves @p out unchanged, for
/// anything else: a hash that is not exactly 64 lower-case hex digits, a decode index with a sign, padding
/// (leading zero) or a value above INT32_MAX, an unknown stage, a metric id outside 1..32 of [A-Za-z0-9_], a
/// tb id with a leading zero or above UINT32_MAX, an unknown plane, or extra characters.
// SPEC-ASSUMPTION(#98): "tb-" ids follow the no-padding rule of decode-index, so parse and format round-trip.
[[nodiscard]] bool parse_blob_id(std::string_view text, BlobIdParts& out);

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_UNITS_HPP
