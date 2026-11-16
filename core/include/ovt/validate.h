// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/validate.h
 * @brief The FrameAnalysis / StreamIndex validator of chapter 06 §6.9 as C entry points (chapter 12 §12.3.8,
 *        WI-0.086 step 7). Thin wrappers of ovt::model::validate_frame_analysis / validate_stream_index.
 *
 * Both functions hand back the findings as a `DiagnosticList` FlatBuffer (ovt_model.fbs, identifier "OVTD",
 * not size-prefixed): one `Diagnostic` per broken rule with `code = "MDL-VAL-0nnn"`, the rule's severity, a
 * `message` that begins with `"V-nnn: "` and ends with ` (at <path>)` when the rule names a location, and
 * `spec_ref = "chapter 06 §6.9"`. An empty list means the buffer is valid. A buffer that does not pass the
 * FlatBuffers verifier is reported as `V-001` (`MDL-VAL-0001`), not as a status code: the status only says
 * whether the call itself could run. SPEC-ASSUMPTION(#674): the table, the Diagnostic form and the status.
 */
#ifndef OVT_VALIDATE_H
#define OVT_VALIDATE_H

#include <stddef.h> /* NOLINT(modernize-deprecated-headers): a C11 header */
#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Validates one `FrameAnalysis` buffer (identifier "OVTA", plain or size-prefixed) against rules V-001…V-029
 * of chapter 06 §6.9 and returns every violation (no early exit; the per-rule cap of 64 summarises the rest).
 *
 * @param fb                   The buffer; owned by the caller, only read during the call. NULL is
 *                             OVT_E_INVALID_ARG; an empty buffer reports V-001.
 * @param len                  Its size in bytes.
 * @param expected_source_hash NULL, or 32 bytes the record's `source_hash` must equal (rule V-023 checks only
 *                             the blob_id grammar without it). Owned by the caller.
 * @param codec_override       -1 for none, else an `ovt.fb.Codec` value that stands in for the record's own
 *                             codec. Any other value is OVT_E_INVALID_ARG.
 * @param out                  Receives the `DiagnosticList`; owned by the caller and released with
 *                             ovt_buffer_free() (chapter 12 §12.3.1 rule 3a). Zeroed when the call fails.
 * @return OVT_OK when the validator ran (also when it found violations), OVT_E_INVALID_ARG for a NULL @p fb
 *         or @p out or a bad @p codec_override, OVT_E_NOMEM, OVT_E_INTERNAL.
 */
OVT_API ovt_status ovt_validate_frame_analysis(const uint8_t* fb, size_t len, const uint8_t* expected_source_hash,
                                               int codec_override, ovt_Buffer* out);

/**
 * Validates one `StreamIndex` buffer (plain or size-prefixed, with or without the cache's "OVTI" identifier)
 * against rule V-031 of chapter 06 §6.9 (plus V-001 and the diagnostics grammar).
 *
 * @param fb  The buffer; owned by the caller, only read during the call. NULL is OVT_E_INVALID_ARG; an empty
 *            buffer reports V-001.
 * @param len Its size in bytes.
 * @param out Receives the `DiagnosticList`; owned by the caller and released with ovt_buffer_free(). Zeroed
 *            when the call fails.
 * @return OVT_OK when the validator ran (also when it found violations), OVT_E_INVALID_ARG for a NULL @p fb
 *         or @p out, OVT_E_NOMEM, OVT_E_INTERNAL.
 */
OVT_API ovt_status ovt_validate_stream_index(const uint8_t* fb, size_t len, ovt_Buffer* out);

#ifdef __cplusplus
}
#endif

#endif /* OVT_VALIDATE_H */
