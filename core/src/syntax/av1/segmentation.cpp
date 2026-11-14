// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 segmentation_params() (chapter 7 §7.3.6 row 17; WI-1.117): the per-segment feature tables with the
// Segmentation_Feature_Bits / _Signed / _Max constants and the SegIdPreSkip / LastActiveSegId derivations.

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecSegmentation = "AV1 5.9.14";

// The feature loop of AV1 §5.9.14 for segmentation_update_data == 1.
void parse_feature_tables(SyntaxBuilder& b, SegmentationParams& seg) {
  for (int i = 0; i < kMaxSegments; ++i) {
    const auto si = static_cast<size_t>(i);
    b.begin_array_element("segment", i, kSpecSegmentation);
    for (int j = 0; j < kSegLvlMax; ++j) {
      const auto sj = static_cast<size_t>(j);
      int32_t feature_value = 0;
      b.begin_array_element("feature", j, kSpecSegmentation);
      const bool feature_enabled = b.read_flag("feature_enabled");
      b.set_enum(segmentation_feature_name(j));
      seg.FeatureEnabled[si][sj] = feature_enabled;
      int32_t clippedValue = 0;
      if (feature_enabled) {
        const unsigned bitsToRead = kSegmentationFeatureBits[sj];
        const int32_t limit = kSegmentationFeatureMax[sj];
        if (kSegmentationFeatureSigned[sj]) {
          feature_value = static_cast<int32_t>(b.read_su("feature_value", 1U + bitsToRead));
          b.set_descriptor_text(fmt::format("su(1+{})", bitsToRead));
          clippedValue = std::clamp(feature_value, -limit, limit);
        } else if (bitsToRead > 0) {
          feature_value = static_cast<int32_t>(b.read_u("feature_value", bitsToRead));
          clippedValue = std::clamp(feature_value, 0, limit);
        } else {
          // SEG_LVL_SKIP and SEG_LVL_GLOBALMV carry no value (0 bits): feature_value = 0.
          b.derived("feature_value", 0);
        }
      }
      seg.FeatureData[si][sj] = static_cast<int16_t>(clippedValue);
      b.end_array_element();
    }
    b.end_array_element();
  }
}

void derive_segment_ids(SegmentationParams& seg) noexcept {
  // AV1 §5.9.14, after the tables: SegIdPreSkip and LastActiveSegId.
  seg.SegIdPreSkip = false;
  seg.LastActiveSegId = 0;
  for (int i = 0; i < kMaxSegments; ++i) {
    for (int j = 0; j < kSegLvlMax; ++j) {
      if (seg.FeatureEnabled[static_cast<size_t>(i)][static_cast<size_t>(j)]) {
        seg.LastActiveSegId = static_cast<uint8_t>(i);
        if (j >= kSegLvlRefFrame) {
          seg.SegIdPreSkip = true;
        }
      }
    }
  }
}

}  // namespace

std::string_view segmentation_feature_name(int feature) noexcept {
  switch (feature) {
    case 0:
      return "SEG_LVL_ALT_Q";
    case 1:
      return "SEG_LVL_ALT_LF_Y_V";
    case 2:
      return "SEG_LVL_ALT_LF_Y_H";
    case 3:
      return "SEG_LVL_ALT_LF_U";
    case 4:
      return "SEG_LVL_ALT_LF_V";
    case 5:
      return "SEG_LVL_REF_FRAME";
    case 6:
      return "SEG_LVL_SKIP";
    case 7:
      return "SEG_LVL_GLOBALMV";
    default:
      return "";
  }
}

SegmentationParams parse_segmentation_params(BitReader& br, SyntaxBuilder& b, uint8_t primary_ref_frame,
                                             const FeatureEnabledTable& prev_enabled,
                                             const FeatureDataTable& prev_data) {
  // AV1 §5.9.14
  SegmentationParams seg;
  const auto start = br.mark();
  auto s = b.scoped("segmentation_params", kSpecSegmentation);
  seg.segmentation_enabled = b.read_flag("segmentation_enabled");
  if (seg.segmentation_enabled) {
    if (primary_ref_frame == kPrimaryRefNone) {
      seg.segmentation_update_map = true;
      seg.segmentation_temporal_update = false;
      seg.segmentation_update_data = true;
      b.derived("segmentation_update",
                "segmentation_update_map = 1, segmentation_temporal_update = 0, segmentation_update_data = 1 "
                "(primary_ref_frame = PRIMARY_REF_NONE)");
    } else {
      seg.segmentation_update_map = b.read_flag("segmentation_update_map");
      if (seg.segmentation_update_map) {
        seg.segmentation_temporal_update = b.read_flag("segmentation_temporal_update");
      } else {
        seg.segmentation_temporal_update = false;
      }
      seg.segmentation_update_data = b.read_flag("segmentation_update_data");
    }
    if (seg.segmentation_update_data) {
      parse_feature_tables(b, seg);
    } else {
      // The tables of load_previous() (or setup_past_independence(): zeros) stay in force.
      seg.FeatureEnabled = prev_enabled;
      seg.FeatureData = prev_data;
      b.derived("feature_tables", "FeatureEnabled / FeatureData kept from the primary reference frame");
    }
  } else {
    seg.FeatureEnabled = FeatureEnabledTable{};
    seg.FeatureData = FeatureDataTable{};
  }
  derive_segment_ids(seg);
  b.derived("segment_ids",
            fmt::format("SegIdPreSkip = {}, LastActiveSegId = {}", seg.SegIdPreSkip ? 1 : 0, seg.LastActiveSegId));
  seg.span = br.span_since(start);
  return seg;
}

}  // namespace ovt::syntax::av1
