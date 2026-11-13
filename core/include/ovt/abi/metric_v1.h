/* SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
 * SPDX-License-Identifier: BSD-2-Clause */
/**
 * @file ovt/abi/metric_v1.h
 * @brief Metric plugin ABI, version 1 (chapter 0 §0.6.3, chapter 9 §9.2.2).
 *
 * A metric plugin is a shared library `libovt_metric_<id>.so` / `.dylib` exporting only ovt_metric_v1().
 * Plugins do not link libovt-core, so this header is self-contained and BSD-2-Clause (chapter 15 §15.1): it
 * includes no LGPL header and no C++ or FlatBuffers header. It is never modified once published (chapter 12
 * §12.3.2); a change makes metric_v2.h. The public-layout structs begin with `uint32_t struct_size` (D4): the
 * writer sets sizeof() of the struct it was compiled with, the reader checks it before reading any member.
 *
 * Rules, binding on every plugin, in-tree or third-party (chapter 9 §9.2.2):
 *  1. one ovt_Metric instance is used by exactly one thread; the host creates one instance per (job, metric,
 *     chunk);
 *  2. score() is called with frames in increasing id.decode_index order within an instance; a plugin MAY keep
 *     state (motion features need the previous frame);
 *  3. score() MAY return out->data == NULL (a chunked metric that emits one FrameScore every N frames,
 *     descriptor.temporal_unit_frames = N); the host then expects the chunk's score on a later call or from
 *     finish();
 *  4. the FrameScore returned is owned by the plugin until the next score()/finish()/destroy() call on that
 *     instance; the host copies it immediately;
 *  5. plugins MUST NOT write to the ref/dist sample buffers;
 *  6. options_json is a JSON object; unknown keys MUST be rejected with OVT_E_INVALID_ARG;
 *  7. every entry point is noexcept: a C++ plugin catches every exception and returns OVT_E_PLUGIN.
 */
#ifndef OVT_ABI_METRIC_V1_H
#define OVT_ABI_METRIC_V1_H

/* A C11 header (chapter 12 §12.3.1 rule 1): C typedefs, C headers, C arrays and (void) prototypes on purpose. */
/* NOLINTBEGIN(modernize-use-using,modernize-deprecated-headers,modernize-redundant-void-arg,modernize-avoid-c-arrays)
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ovt_status and ovt_Buffer, identical to ovt/status.h (chapter 12 §12.3.3). They are repeated here so that
 * this header needs no LGPL header; the shared guard makes either include order work (#211, as in
 * ovt/abi/analysis_decoder_v1.h). A CTest check keeps the copies textually identical. */
#ifndef OVT_STATUS_TYPES_DEFINED
#define OVT_STATUS_TYPES_DEFINED
/** Result of every fallible call. Values are never renumbered; new values are appended. */
typedef enum ovt_status {
  OVT_OK = 0,
  OVT_E_INVALID_ARG,
  OVT_E_IO,
  OVT_E_UNSUPPORTED,
  OVT_E_CORRUPT,
  OVT_E_NOMEM,
  OVT_E_CANCELLED,
  OVT_E_PLUGIN,
  OVT_E_INTERNAL
} ovt_status;

/** A byte span. Exempt from the struct_size rule (chapter 12 §12.3.1 rule 7); never extended. */
typedef struct ovt_Buffer {
  const uint8_t* data;
  size_t size;
} ovt_Buffer;
#endif /* OVT_STATUS_TYPES_DEFINED */

/* ovt_FrameId, identical to ovt/decode.h (backbone §0.5.3); same shared-guard scheme. */
#ifndef OVT_FRAME_ID_DEFINED
#define OVT_FRAME_ID_DEFINED
/**
 * Identity of a frame in a stream (backbone §0.5.3). A value type: never extended, no struct_size
 * (§12.3.1 rule 7). @c display_index is -1 for a frame that is never shown.
 */
typedef struct ovt_FrameId {
  int32_t decode_index;
  int32_t display_index;
} ovt_FrameId;
#endif /* OVT_FRAME_ID_DEFINED */

/* ovt_ColorDescription, identical to ovt/color.h (backbone §0.5.6); same shared-guard scheme. */
#ifndef OVT_COLOR_DESCRIPTION_DEFINED
#define OVT_COLOR_DESCRIPTION_DEFINED
/**
 * Colour description of a picture or stream (backbone §0.5.6). Code points are ITU-T H.273 values; 0 in a
 * static HDR metadata field means "absent". Frozen: ovt_Picture and ovt_MetricPlanes embed it by value (#156).
 */
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

/** The ABI version this header describes; ovt_MetricApi.abi_version MUST equal it. */
#define OVT_METRIC_ABI_VERSION 1

/* Values of ovt_MetricPlanes.domain: the ovt.fb.WorkingDomain enumerators of core/schema/ovt_metrics.fbs
 * (backbone §0.5.7), spelled out for plugins written in C. */
#define OVT_METRIC_DOMAIN_CODE_VALUE 0   /**< integer Y'CbCr (or G'B'R') code values at source subsampling */
#define OVT_METRIC_DOMAIN_LINEAR_LIGHT 1 /**< float32 R,G,B absolute luminance in cd/m², 4:4:4 */
#define OVT_METRIC_DOMAIN_PU21 2         /**< float32 PU21 banding_glare encoded values */
#define OVT_METRIC_DOMAIN_ICTCP 3        /**< float32 BT.2100 ICtCp (PQ), 4:4:4 */
#define OVT_METRIC_DOMAIN_MODEL_RGB 4    /**< float32 model-specific normalised R,G,B planes */

/* Values of ovt_MetricPlanes.sample_format. */
#define OVT_METRIC_SAMPLE_UINT 0    /**< unsigned integer, little-endian uint16 when sample_bytes == 2 */
#define OVT_METRIC_SAMPLE_FLOAT32 1 /**< IEEE 754 binary32 */

/** One metric instance, opaque to the host. Created by create(), released by destroy(). */
typedef struct ovt_Metric ovt_Metric;

/**
 * The planes of one normalised picture as delivered to score() (chapter 9 §9.2.2; the host converts both
 * streams into descriptor.domain first, §9.2.4). Read-only for the plugin; valid only during the call.
 * Sample layout per domain (chapter 9 §9.2.2 table):
 *  - CodeValue: 1 or 3 planes, unsigned integers, bit_depth 8/10/12/16, sample_bytes 1 or 2 (little-endian),
 *    chroma at the source subsampling unless chroma upsampling was requested;
 *  - LinearLight, ICtCp, ModelRGB: 3 planes, 4:4:4, float32;
 *  - PU21: 3 planes (R,G,B) or 1 (luminance), float32.
 * Named ovt_MetricPlanes because ovt_PlaneView (ovt/decode.h) is the single-plane view of chapter 12 §12.3.9
 * (#156).
 */
typedef struct ovt_MetricPlanes {
  uint32_t struct_size;       /**< sizeof(ovt_MetricPlanes) as compiled into the host (D4) */
  const uint8_t* data[3];     /**< Y, U, V (or R,G,B / I,Ct,Cp); data[1], data[2] NULL for 4:0:0 */
  int32_t stride_bytes[3];    /**< row pitch per plane in bytes */
  uint32_t width[3];          /**< per plane, in samples */
  uint32_t height[3];         /**< per plane, in rows */
  uint8_t planes;             /**< 1 or 3 */
  uint8_t bit_depth;          /**< 8..16 for integer samples; 32 for float */
  uint8_t sample_bytes;       /**< 1, 2 or 4 */
  uint8_t sample_format;      /**< OVT_METRIC_SAMPLE_UINT or OVT_METRIC_SAMPLE_FLOAT32 */
  uint8_t domain;             /**< ovt.fb.WorkingDomain (OVT_METRIC_DOMAIN_*) */
  ovt_ColorDescription color; /**< backbone §0.5.6; describes the samples as delivered */
  double display_peak_nits;   /**< display-model peak used for linear/pu21/ictcp; 0 for code_value */
} ovt_MetricPlanes;

/** The function table a plugin exports. Static and immutable; owned by the plugin. */
typedef struct ovt_MetricApi {
  uint32_t struct_size; /**< sizeof(ovt_MetricApi) as compiled into the plugin (D4) */
  uint32_t abi_version; /**< OVT_METRIC_ABI_VERSION */
  /** Returns the plugin's ovt.fb.MetricDescriptor as a FlatBuffer (core/schema/ovt_metrics.fbs). The bytes are
   *  owned by the plugin, immutable and valid until the library is unloaded; the host verifies and copies them.
   *  descriptor.id MUST equal <id> of libovt_metric_<id> and match `[a-z0-9_]{1,32}`. */
  ovt_Buffer (*descriptor)(void);
  /** Creates an instance configured by @p options_json (a JSON object, "{}" for defaults; unknown keys are
   *  OVT_E_INVALID_ARG). On success *out is owned by the host and released with destroy(). */
  ovt_status (*create)(const char* options_json, ovt_Metric** out);
  /** Releases an instance created by create(), including any FrameScore it still owns. */
  void (*destroy)(ovt_Metric*);
  /** Scores one frame pair. Frames are delivered already normalised into descriptor.domain; @p ref is NULL for
   *  NoReference metrics. @p out receives a FrameScore FlatBuffer owned by the plugin until the next call on
   *  this instance (rule 4), or data == NULL when this call emits no score (rule 3). */
  ovt_status (*score)(ovt_Metric*, const ovt_MetricPlanes* ref, const ovt_MetricPlanes* dist, ovt_FrameId id,
                      ovt_Buffer* out);
  /** Ends the sequence. @p out receives an optional sequence-level FrameScore (data == NULL when there is
   *  none), owned by the plugin until destroy(). */
  ovt_status (*finish)(ovt_Metric*, ovt_Buffer* out);
} ovt_MetricApi;

/** Default visibility for the plugin's one exported symbol, so plugins can build with hidden visibility. */
#ifndef OVT_PLUGIN_EXPORT
#if defined(__GNUC__) || defined(__clang__)
#define OVT_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define OVT_PLUGIN_EXPORT
#endif
#endif

/**
 * The only exported symbol of a metric plugin (declared here, not in chapter 12 §12.3: plugins do not link
 * libovt-core). Returns the plugin's static function table, owned by the plugin and valid until the library
 * is unloaded; never NULL for a working plugin. noexcept.
 */
OVT_PLUGIN_EXPORT const ovt_MetricApi* ovt_metric_v1(void);

#ifdef __cplusplus
}
#endif
/* NOLINTEND(modernize-use-using,modernize-deprecated-headers,modernize-redundant-void-arg,modernize-avoid-c-arrays) */

#endif /* OVT_ABI_METRIC_V1_H */
