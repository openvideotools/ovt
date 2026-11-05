// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Motion-vector unit conversion (chapter 06 §6.2.3) and blob_id build/parse (§6.6.2), WI-0.084. The parser
// is a hand-written matcher for the ABNF (no regex library).

#include "ovt/cxx/model/units.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ovt::model {

namespace {

constexpr std::int32_t kH26xMvFactor = 4;  // quarter luma sample -> 1/16 (§6.2.3 table)
constexpr std::int32_t kAv1MvFactor = 2;   // eighth luma sample -> 1/16

constexpr std::size_t kHashHexDigits = 64;
constexpr std::size_t kMaxMetricIdLength = 32;
constexpr std::string_view kMapPrefix = "map-";
constexpr std::string_view kTbPrefix = "tb-";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::array<char, 4> kPlaneTokens = {'y', 'u', 'v', 'a'};

// §6.6.2: stage tokens in PlaneStage order (Reconstruction = 0 … Coefficients = 6).
constexpr std::array<std::string_view, 7> kStageTokens = {"recon",      "pred",  "resid", "predeblock",
                                                          "presaocdef", "prelr", "coeff"};

constexpr bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

constexpr bool is_metric_id_char(char c) noexcept {
  return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_valid_metric_id(std::string_view id) noexcept {
  return !id.empty() && id.size() <= kMaxMetricIdLength && std::ranges::all_of(id, is_metric_id_char);
}

// 1*DIGIT without leading zeros ("0" itself is allowed), value <= max.
std::optional<std::uint64_t> parse_unpadded(std::string_view digits, std::uint64_t max) noexcept {
  if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : digits) {
    if (!is_digit(c)) {
      return std::nullopt;
    }
    value = (value * 10) + static_cast<std::uint64_t>(c - '0');
    if (value > max) {
      return std::nullopt;
    }
  }
  return value;
}

std::optional<std::uint8_t> hex_value(char c) noexcept {
  const std::size_t pos = kHexDigits.find(c);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(pos);
}

// Splits off the text before the next '/' and advances @p rest past it; nullopt when there is no '/'.
std::optional<std::string_view> take_segment(std::string_view& rest) noexcept {
  const std::size_t slash = rest.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view segment = rest.substr(0, slash);
  rest.remove_prefix(slash + 1);
  return segment;
}

}  // namespace

// ---- Motion vectors ---------------------------------------------------------------------------------

int mv_factor(fb::Codec codec) {
  switch (codec) {
    case fb::Codec::H264:
    case fb::Codec::H265:
      return kH26xMvFactor;
    case fb::Codec::AV1:
      return kAv1MvFactor;
    default:
      throw std::invalid_argument("ovt::model: no MV unit conversion for this codec (H.264, HEVC and AV1 only)");
  }
}

std::optional<std::int16_t> mv_to_neutral_checked(std::int32_t spec_mv, fb::Codec codec) {
  const std::int64_t neutral = std::int64_t{spec_mv} * mv_factor(codec);
  if (neutral > kNeutralMvMax || neutral < -std::int64_t{kNeutralMvMax}) {
    return std::nullopt;
  }
  return static_cast<std::int16_t>(neutral);
}

std::int16_t mv_to_neutral(std::int32_t spec_mv, fb::Codec codec) {
  const std::int64_t neutral = std::int64_t{spec_mv} * mv_factor(codec);
  return static_cast<std::int16_t>(std::clamp<std::int64_t>(neutral, -kNeutralMvMax, kNeutralMvMax));
}

std::int32_t mv_from_neutral(std::int16_t neutral, fb::Codec codec) {
  const int factor = mv_factor(codec);
  if (neutral % factor != 0) {
    throw std::invalid_argument("ovt::model::mv_from_neutral: value is not a multiple of the codec's factor");
  }
  return neutral / factor;
}

// ---- blob_id ----------------------------------------------------------------------------------------

std::string_view stage_token(fb::PlaneStage stage) noexcept {
  const auto index = static_cast<std::size_t>(stage);
  return index < kStageTokens.size() ? kStageTokens.at(index) : std::string_view{};
}

std::string blob_id(const BlobIdParts& parts) {
  if (parts.decode_index < 0) {
    throw std::invalid_argument("ovt::model::blob_id: decode_index must be >= 0");
  }
  if (parts.plane >= kPlaneTokens.size()) {
    throw std::invalid_argument("ovt::model::blob_id: plane must be 0..3");
  }
  std::string out;
  out.reserve(kHashHexDigits + 32);
  for (const std::uint8_t byte : parts.source_hash) {
    out.push_back(kHexDigits.at(byte >> 4U));
    out.push_back(kHexDigits.at(byte & 0x0FU));
  }
  out.push_back('/');
  out.append(std::to_string(parts.decode_index));
  out.push_back('/');
  switch (parts.kind) {
    case BlobKind::Plane: {
      const std::string_view token = stage_token(parts.stage);
      if (token.empty()) {
        throw std::invalid_argument("ovt::model::blob_id: stage outside ovt.fb.PlaneStage");
      }
      out.append(token);
      break;
    }
    case BlobKind::MetricMap:
      if (!is_valid_metric_id(parts.metric_id)) {
        throw std::invalid_argument("ovt::model::blob_id: metric id must be 1..32 of [A-Za-z0-9_]");
      }
      out.append(kMapPrefix);
      out.append(parts.metric_id);
      break;
    case BlobKind::TransformBlock:
      out.append(kTbPrefix);
      out.append(std::to_string(parts.tb_block_id));
      break;
    default:
      throw std::invalid_argument("ovt::model::blob_id: unknown BlobKind");
  }
  out.push_back('/');
  out.push_back(kPlaneTokens.at(parts.plane));
  return out;
}

std::string blob_id(std::span<const std::uint8_t, 32> hash, int decode_index, fb::PlaneStage stage,
                    std::uint8_t plane) {
  BlobIdParts parts;
  std::ranges::copy(hash, parts.source_hash.begin());
  parts.decode_index = decode_index;
  parts.kind = BlobKind::Plane;
  parts.stage = stage;
  parts.plane = plane;
  return blob_id(parts);
}

bool parse_blob_id(std::string_view text, BlobIdParts& out) {
  BlobIdParts parts;
  std::string_view rest = text;

  // source-hash = 64LOWERHEX
  const auto hash = take_segment(rest);
  if (!hash || hash->size() != kHashHexDigits) {
    return false;
  }
  for (std::size_t i = 0; i < parts.source_hash.size(); ++i) {
    const auto hi = hex_value((*hash)[2 * i]);
    const auto lo = hex_value((*hash)[(2 * i) + 1]);
    if (!hi || !lo) {
      return false;
    }
    parts.source_hash.at(i) = static_cast<std::uint8_t>((*hi << 4U) | *lo);
  }

  // decode-index = 1*DIGIT, no padding, no sign; it is a FrameId.decode_index (int).
  const auto index_text = take_segment(rest);
  if (!index_text) {
    return false;
  }
  const auto index = parse_unpadded(*index_text, std::numeric_limits<std::int32_t>::max());
  if (!index) {
    return false;
  }
  parts.decode_index = static_cast<std::int32_t>(*index);

  // stage
  const auto stage = take_segment(rest);
  if (!stage) {
    return false;
  }
  if (const auto* it = std::ranges::find(kStageTokens, *stage); it != kStageTokens.end()) {
    parts.kind = BlobKind::Plane;
    parts.stage = static_cast<fb::PlaneStage>(it - kStageTokens.begin());
  } else if (stage->starts_with(kMapPrefix)) {
    const std::string_view id = stage->substr(kMapPrefix.size());
    if (!is_valid_metric_id(id)) {
      return false;
    }
    parts.kind = BlobKind::MetricMap;
    parts.metric_id = std::string(id);
  } else if (stage->starts_with(kTbPrefix)) {
    const auto block_id = parse_unpadded(stage->substr(kTbPrefix.size()), std::numeric_limits<std::uint32_t>::max());
    if (!block_id) {
      return false;
    }
    parts.kind = BlobKind::TransformBlock;
    parts.tb_block_id = static_cast<std::uint32_t>(*block_id);
  } else {
    return false;
  }

  // plane = "y" / "u" / "v" / "a", and nothing after it.
  if (rest.size() != 1) {
    return false;
  }
  const auto* plane = std::ranges::find(kPlaneTokens, rest.front());
  if (plane == kPlaneTokens.end()) {
    return false;
  }
  parts.plane = static_cast<std::uint8_t>(plane - kPlaneTokens.begin());

  out = std::move(parts);
  return true;
}

}  // namespace ovt::model
