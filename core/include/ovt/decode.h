// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/decode.h
 * @brief Decoded pictures and their plane views (chapter 12 §12.3.9, chapter 06 §6.6.1).
 *
 * WI-0.087 adds the view types ovt_PlaneView, ovt_FrameId and ovt_Picture. The reference counting of
 * ovt_Picture (ovt_picture_ref/ovt_picture_release, D4 b), the decoder handle (ovt_Decoder, ovt_decoder_*)
 * and ovt_picture_export_plane arrive with the work items that implement them.
 *
 * Plane memory rules (§6.6.1): planar only; 8-bit samples are one byte, 9–16-bit samples are little-endian
 * 16-bit words with the value in the low bit_depth bits; Residual samples are signed 16-bit, Coefficients
 * signed 32-bit. Rows are stride_bytes apart; bytes past width * sample_bytes in a row are padding with
 * unspecified values.
 */
/* SPEC-ASSUMPTION(#156): the layouts are those of chapter 12 §12.3.9, a superset of backbone
   §0.6.1; the multi-plane ovt_PlaneView of chapter 09 §9.2.2 is a different type that needs another name. */
#ifndef OVT_DECODE_H
#define OVT_DECODE_H

#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#include "ovt/color.h"
#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-avoid-c-arrays): a C11 header */
/**
 * One plane of a picture (chapter 12 §12.3.9). Memory pointed to by @c data is owned by the picture or
 * store entry the view came from and is read-only for the caller.
 */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_PlaneView {
  uint32_t struct_size;  /**< sizeof(ovt_PlaneView) (§12.3.1 rule 7); set by the library. */
  const uint8_t* data;   /**< First byte of row 0; NULL for an absent plane. */
  uint32_t width;        /**< In samples of this plane (chroma planes already subsampled). */
  uint32_t height;       /**< In rows of this plane. */
  uint32_t stride_bytes; /**< Row pitch in bytes, >= width * sample_bytes; always use it. */
  uint8_t bit_depth;     /**< 8..16 for samples, 16 for Residual, 32 for Coefficients. */
  uint8_t sample_bytes;  /**< 1 or 2 (little-endian), 4 for Coefficients (int32). */
  uint8_t is_signed;     /**< 1 for Residual and Coefficients, else 0. */
  uint8_t plane;         /**< 0 Y, 1 U, 2 V (3 alpha, future). */
  uint8_t stage;         /**< ovt.fb.PlaneStage, or 0xFE for a metric map. */
  uint8_t reserved[3];   /**< Zero. */
} ovt_PlaneView;

/* ovt/abi/metric_v1.h repeats this type under the same guard (#156, #211). */
#ifndef OVT_FRAME_ID_DEFINED
#define OVT_FRAME_ID_DEFINED
/**
 * Identity of a frame in a stream (backbone §0.5.3). A value type: never extended, no struct_size
 * (§12.3.1 rule 7). @c display_index is -1 for a frame that is never shown.
 */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_FrameId {
  int32_t decode_index;
  int32_t display_index;
} ovt_FrameId;
#endif /* OVT_FRAME_ID_DEFINED */

/**
 * A decoded picture (chapter 12 §12.3.9). Reference counted (D4 b): a function that hands one out through an
 * out-parameter gives the caller one reference, released with ovt_picture_release(). The struct and its
 * planes stay valid, unchanged, until the last reference is released, whatever else happens to the handle
 * that produced it. Every member is read-only for the caller.
 */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_Picture {
  uint32_t struct_size;       /**< sizeof(ovt_Picture) (§12.3.1 rule 7); set by the library. */
  ovt_PlaneView planes[3];    /**< Y, U, V; planes[1..2] are all zero for 4:0:0. */
  uint8_t chroma_format;      /**< 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4. */
  ovt_ColorDescription color; /**< As signalled for this picture. */
  ovt_FrameId id;             /**< Frame identity. */
  int64_t pts;                /**< Presentation time in timebase units. */
  int32_t timebase_num;       /**< Timebase numerator; 0 when unknown. */
  int32_t timebase_den;       /**< Timebase denominator; 0 when unknown. */
  int32_t crop_x;             /**< Conformance window, in luma samples; planes[] are the uncropped picture. */
  int32_t crop_y;             /**< See crop_x. */
  int32_t crop_w;             /**< See crop_x. */
  int32_t crop_h;             /**< See crop_x. */
  uint8_t hw_decoded;         /**< 1 if produced by a hardware decoder (not bit-exact across devices). */
  void* opaque;               /**< Library private: the plane memory owner and the reference count. */
} ovt_Picture;

/* NOLINTEND(modernize-avoid-c-arrays) */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OVT_DECODE_H */
