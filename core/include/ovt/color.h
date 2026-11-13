// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/color.h
 * @brief Colour description shared by decode, metrics and render (backbone §0.5.6, chapter 12 §12.3.12).
 *
 * Only ovt_ColorDescription is declared so far: ovt/decode.h embeds it in ovt_Picture (WI-0.087). The
 * functions of §12.3.12 (ovt_hdr_measure, ovt_color_to_json, …) are added by their work items.
 */
/* SPEC-ASSUMPTION(#156): no work item produces ovt/color.h; WI-0.087 creates it with the §0.5.6
   struct only, because ovt_Picture (chapter 12 §12.3.9) contains an ovt_ColorDescription. */
#ifndef OVT_COLOR_H
#define OVT_COLOR_H

#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-avoid-c-arrays): a C11 header */
/* ovt/abi/metric_v1.h repeats this struct under the same guard (#156, #211). */
#ifndef OVT_COLOR_DESCRIPTION_DEFINED
#define OVT_COLOR_DESCRIPTION_DEFINED
/**
 * Colour description of a picture or stream (backbone §0.5.6). Code points are ITU-T H.273 values; 0 in a
 * static HDR metadata field means "absent". Frozen: ovt_Picture and ovt_MetricPlanes embed it by value (#156).
 */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_ColorDescription {
  uint32_t struct_size;          /**< sizeof(ovt_ColorDescription) = 44; set by the writer of the struct. */
  uint8_t primaries;             /**< H.273 ColourPrimaries: 1 BT.709, 9 BT.2020, 12 P3-D65, … */
  uint8_t transfer;              /**< H.273 TransferCharacteristics: 1 BT.709, 13 sRGB, 16 PQ, 18 HLG, … */
  uint8_t matrix;                /**< H.273 MatrixCoefficients: 1 BT.709, 9 BT.2020-NCL, 14 ICtCp, … */
  uint8_t full_range;            /**< 0 limited, 1 full. */
  uint8_t chroma_siting;         /**< 0 unknown, 1 left (MPEG-2), 2 center (MPEG-1/JPEG), 3 top-left (HEVC/AV1). */
  uint8_t chroma_format;         /**< 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4. */
  uint8_t bit_depth;             /**< Luma bit depth. */
  uint16_t mdcv_primaries[3][2]; /**< ST 2086 display primaries (x, y), 0.00002 units; 0 = absent. */
  uint16_t mdcv_white_point[2];  /**< ST 2086 white point (x, y), 0.00002 units; 0 = absent. */
  uint32_t mdcv_max_luminance;   /**< ST 2086 max luminance, 0.0001 cd/m² units; 0 = absent. */
  uint32_t mdcv_min_luminance;   /**< ST 2086 min luminance, 0.0001 cd/m² units; 0 = absent. */
  uint16_t max_cll;              /**< CTA-861.3 MaxCLL, cd/m²; 0 = absent. */
  uint16_t max_fall;             /**< CTA-861.3 MaxFALL, cd/m²; 0 = absent. */
  uint8_t has_hdr10plus;         /**< 1 when HDR10+ dynamic metadata was found. */
  uint8_t has_dolby_vision;      /**< 1 when a Dolby Vision configuration was found. */
  uint8_t dv_profile;            /**< Dolby Vision profile; 0 when has_dolby_vision is 0. */
  uint8_t dv_level;              /**< Dolby Vision level; 0 when has_dolby_vision is 0. */
} ovt_ColorDescription;
#endif /* OVT_COLOR_DESCRIPTION_DEFINED */

/* NOLINTEND(modernize-avoid-c-arrays) */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OVT_COLOR_H */
