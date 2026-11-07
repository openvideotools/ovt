/* SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
 * SPDX-License-Identifier: BSD-2-Clause */
/**
 * @file ovt/abi/analysis_decoder_v1.h
 * @brief Analysis decoder plugin ABI, version 1 (chapter 0 §0.6.2, chapter 7 §7.5.1 and §7.6).
 *
 * A plugin is a shared library `libovt_plugin_<id>.so` / `.dylib` exporting only ovt_analysis_decoder_v1().
 * Plugins do not link libovt-core, so this header is self-contained and BSD-2-Clause (chapter 15 §15.1).
 * It is never modified once published (chapter 12 §12.3.2); a change makes analysis_decoder_v2.h. Both
 * structs begin with `uint32_t struct_size` (D4): the plugin sets sizeof() of the struct it was compiled
 * with, and the host checks it before reading any member. Rules (chapter 0 §0.6.2, chapter 7 §7.6):
 *  - every entry point is noexcept: a C++ plugin catches every exception and returns OVT_E_PLUGIN;
 *  - one decoder instance is driven by one thread at a time (the host never calls it concurrently);
 *  - malformed input never crashes the plugin: it returns OVT_E_CORRUPT with a Diagnostic;
 *  - FrameAnalysis.frame.id.decode_index is filled from the `decode_index_hint` given to push();
 *  - every per-block syntax element the decoder reads appears in the codec extension table.
 */
#ifndef OVT_ABI_ANALYSIS_DECODER_V1_H
#define OVT_ABI_ANALYSIS_DECODER_V1_H

/* A C11 header (chapter 12 §12.3.1 rule 1): C typedefs, C headers and (void) prototypes on purpose. */
/* NOLINTBEGIN(modernize-use-using,modernize-deprecated-headers,modernize-redundant-void-arg) */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ovt_status and ovt_Buffer, identical to ovt/status.h (chapter 12 §12.3.3). They are repeated here so that
 * this header needs no LGPL header; the shared guard makes either include order work.
 * SPEC-ASSUMPTION(#211): a self-contained ABI header with a shared guard. */
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

/** The ABI version this header describes; ovt_AnalysisDecoderApi.abi_version MUST equal it. */
#define OVT_ANALYSIS_ABI_VERSION 1

/* Bits of ovt_AnalysisCaps.grid_fields: which CellGrid vectors the plugin fills (chapter 7 §7.6). A vector
 * not announced MUST be empty in the FlatBuffer. SPEC-ASSUMPTION(#211): the macro names. */
#define OVT_GRID_BLOCK_ID (1u << 0)     /**< block_id */
#define OVT_GRID_PARTITION_ID (1u << 1) /**< partition_id */
#define OVT_GRID_PRED (1u << 2)         /**< pred */
#define OVT_GRID_QP (1u << 3)           /**< qp */
#define OVT_GRID_INTRA_MODE_Y (1u << 4) /**< intra_mode_y */
#define OVT_GRID_INTRA_MODE_C (1u << 5) /**< intra_mode_c */
#define OVT_GRID_MV0 (1u << 6)          /**< mv0_x, mv0_y */
#define OVT_GRID_MV1 (1u << 7)          /**< mv1_x, mv1_y */
#define OVT_GRID_REF0 (1u << 8)         /**< ref0 */
#define OVT_GRID_REF1 (1u << 9)         /**< ref1 */
#define OVT_GRID_TX_LOG2 (1u << 10)     /**< tx_log2_w, tx_log2_h */
#define OVT_GRID_TX_TYPE (1u << 11)     /**< tx_type */
#define OVT_GRID_CBF (1u << 12)         /**< cbf */
#define OVT_GRID_BITS_X8 (1u << 13)     /**< bits_x8 */
#define OVT_GRID_DEBLOCK_BS (1u << 14)  /**< deblock_bs_v, deblock_bs_h */
#define OVT_GRID_FILTER_IDX (1u << 15)  /**< filter_idx */

/** One decoder instance, opaque to the host. Created by create(), released by destroy(). */
typedef struct ovt_AnalysisDecoder ovt_AnalysisDecoder;

/** What a plugin can do. Static and immutable for the plugin's lifetime; owned by the plugin. */
typedef struct ovt_AnalysisCaps {
  uint32_t struct_size;          /**< sizeof(ovt_AnalysisCaps) as compiled into the plugin (D4) */
  uint8_t codec;                 /**< ovt.fb.Codec value of the one codec this plugin decodes */
  uint32_t plane_stages;         /**< bitmask of ovt.fb.PlaneStage (bit = enum value) it can export */
  uint32_t grid_fields;          /**< bitmask of OVT_GRID_* bits: which CellGrid vectors are filled */
  uint8_t has_symbol_accounting; /**< 1 if FrameAnalysis.symbols is filled */
  uint8_t bit_exact;             /**< 1 if reconstruction is conformance-exact */
  uint8_t supports_threads;      /**< 1 if the plugin's internal threads are safe (not re-entrancy) */
  const char* id;                /**< `[a-z0-9_]{1,32}`, equal to <id> in libovt_plugin_<id>; non-null */
  const char* version;           /**< plugin version (part of the analysis cache key); non-null */
  const char* license;           /**< SPDX expression of the plugin binary; non-null */
  const char* notes;             /**< free text for `ovt plugins`; may be NULL */
} ovt_AnalysisCaps;

/** The function table a plugin exports. Static and immutable; owned by the plugin. */
typedef struct ovt_AnalysisDecoderApi {
  uint32_t struct_size; /**< sizeof(ovt_AnalysisDecoderApi) as compiled into the plugin (D4) */
  uint32_t abi_version; /**< OVT_ANALYSIS_ABI_VERSION */
  /** Returns the plugin's capabilities; static, immutable for the plugin's lifetime, never freed. */
  const ovt_AnalysisCaps* (*caps)(void);
  /** Creates a decoder. @p codec_private (avcC/hvcC/av1C bytes, or NULL for elementary streams) is valid
   *  only during this call: the plugin copies what it needs. @p threads is the worker-thread budget.
   *  On success *out is owned by the host and released with destroy(). */
  ovt_status (*create)(const uint8_t* codec_private, size_t len, int threads, ovt_AnalysisDecoder** out);
  /** Releases a decoder created by create(), including every buffer not yet released. */
  void (*destroy)(ovt_AnalysisDecoder*);
  /** Pushes one Unit (an Annex B NAL unit without start code, or one OBU with its size field). The bytes
   *  are valid only during the call. Returns OVT_OK (frames come out of pull()) or OVT_E_CORRUPT. */
  ovt_status (*push)(ovt_AnalysisDecoder*, const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts);
  /** Pulls the next completed frame as a FrameAnalysis FlatBuffer in *out_fb, owned by the plugin until
   *  release(). Returns OVT_OK with out_fb->data == NULL when no frame is ready. @p plane_stage_mask
   *  selects the ovt.fb.PlaneStage planes to export for this frame. */
  ovt_status (*pull)(ovt_AnalysisDecoder*, uint32_t plane_stage_mask, ovt_Buffer* out_fb);
  /** Looks up a plane named by FrameAnalysis.planes[].blob_id. *data stays owned by the plugin and is
   *  valid until the FrameAnalysis that names it is released. */
  ovt_status (*plane)(ovt_AnalysisDecoder*, const char* blob_id, const uint8_t** data, size_t* size);
  /** Returns a buffer obtained from pull() to the plugin. */
  void (*release)(ovt_AnalysisDecoder*, ovt_Buffer fb);
  /** Signals end of input so that delayed frames come out of pull(). The host destroys the decoder after a
   *  flush; it is never pushed to again. */
  ovt_status (*flush)(ovt_AnalysisDecoder*);
} ovt_AnalysisDecoderApi;

/** Default visibility for the plugin's one exported symbol, so plugins can build with hidden visibility. */
#ifndef OVT_PLUGIN_EXPORT
#if defined(__GNUC__) || defined(__clang__)
#define OVT_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define OVT_PLUGIN_EXPORT
#endif
#endif

/**
 * The only exported symbol of an analysis plugin (declared here, not in chapter 12 §12.3: plugins do not
 * link libovt-core). Returns the plugin's static function table, owned by the plugin and valid until the
 * library is unloaded; never NULL for a working plugin. noexcept.
 */
OVT_PLUGIN_EXPORT const ovt_AnalysisDecoderApi* ovt_analysis_decoder_v1(void);

#ifdef __cplusplus
}
#endif
/* NOLINTEND(modernize-use-using,modernize-deprecated-headers,modernize-redundant-void-arg) */

#endif /* OVT_ABI_ANALYSIS_DECODER_V1_H */
