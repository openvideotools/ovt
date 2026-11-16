// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/syntax.h
 * @brief Stateless conversions of chapter 12 §12.3.8: the generic FlatBuffer <-> JSON pair of chapter 06 §6.8.1
 *        (WI-0.088, `ovt::model::to_json` / `from_json`) behind the C ABI. The `ovt_syntax_*` parsing and
 *        trace functions of the same block are added by their work items.
 *
 * Both functions take a root table name of one of the three schemas (ovt_model.fbs, ovt_container.fbs,
 * ovt_metrics.fbs), with or without the `ovt.fb.` prefix: "FrameAnalysis", "StreamIndex", "SyntaxNode",
 * "ovt.fb.MetricsReport", ... An unknown name is OVT_E_INVALID_ARG. SPEC-ASSUMPTION(#674): the name
 * lookup, the status codes and the text form.
 */
#ifndef OVT_SYNTAX_H
#define OVT_SYNTAX_H

#include <stddef.h>
#include <stdint.h>

#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The JSON text of a finished, not size-prefixed FlatBuffer (chapter 06 §6.8.1 field rules; RFC 8259 text, NaN
 * and the infinities as the strings "nan", "inf", "-inf", chapter 12 §12.1.4).
 *
 * @param fb        The buffer; owned by the caller, only read during the call.
 * @param len       Its size in bytes.
 * @param root_type Name of its root table (see the file comment). Owned by the caller.
 * @param pretty    Non-zero: 2-space indentation, one field per line, a final newline. Zero: one line.
 * @param out_text  Receives the UTF-8 text; `size` excludes a terminating NUL that the buffer also holds, so
 *                  `data` reads as a C string. Owned by the caller and released with ovt_buffer_free()
 *                  (chapter 12 §12.3.1 rule 3a). Zeroed when the call fails.
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL pointer or an unknown @p root_type; OVT_E_CORRUPT when the
 *         buffer does not verify as @p root_type (or breaks one of the §6.8.1 limits); OVT_E_NOMEM;
 *         OVT_E_INTERNAL.
 */
OVT_API ovt_status ovt_fb_to_json(const uint8_t* fb, size_t len, const char* root_type, int pretty,
                                  ovt_Buffer* out_text);

/**
 * The reverse of ovt_fb_to_json(): parses JSON text (either layout, numbers plain or quoted, hashes as hex
 * strings or arrays, nested buffers as objects; unknown keys ignored) and returns the canonical FlatBuffer
 * (chapter 06 WI-0.085 canonicalize()), so that importing an export gives the same bytes again. The schema's
 * file identifier is written when @p root_type is the schema's own root type (FrameAnalysis, ContainerRoot,
 * MetricsReport); other tables are written without one. Not size-prefixed (§6.1.1).
 *
 * @param json      NUL-terminated UTF-8 text; owned by the caller, only read during the call.
 * @param root_type Name of the table the text describes (see the file comment). Owned by the caller.
 * @param out_fb    Receives the buffer; owned by the caller and released with ovt_buffer_free(). Zeroed when the
 *                  call fails.
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL pointer or an unknown @p root_type; OVT_E_CORRUPT when the
 *         importer rejects the text (the reason, with line and column, goes to the debug log); OVT_E_NOMEM;
 *         OVT_E_INTERNAL.
 */
OVT_API ovt_status ovt_json_to_fb(const char* json, const char* root_type, ovt_Buffer* out_fb);

#ifdef __cplusplus
}
#endif

#endif /* OVT_SYNTAX_H */
