// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/syntax.h
 * @brief Stateless syntax functions of chapter 12 §12.3.8. WI-1.504 adds ovt_syntax_trace(); the JSON pair
 *        ovt_fb_to_json() / ovt_json_to_fb() (#674) and ovt_syntax_parse_unit() are added by their work items.
 */
#ifndef OVT_SYNTAX_H
#define OVT_SYNTAX_H

#include <stddef.h> /* NOLINT(modernize-deprecated-headers): a C11 header */
#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The trace text of chapter 12 §12.2.3.1 for one parsed unit: one `[ovt]` line per syntax element (bit offset from
 * the start of the unit's RBSP, name, the bits as read, `=`, value, then ` (enum_name)` and ` -> derived` when set),
 * a `[ovt] ---- <title> ----  (<spec_ref>)` line before each syntax structure and a `[ovt] !! <SEVERITY> <code>
 * <message>` line after the element, or structure, that carries a diagnostic, and a `[ovt] <bit offset>  -> <derived>`
 * line for a derived variable that has no bits of its own. The `[ovt] Unit ...` line that
 * names the unit is the caller's (it needs the Unit record). Deterministic (§0.7.5).
 *
 * The bits are rebuilt from the descriptor, the value and the span: `u(n)`, `f(n)`, `b(8)`, `i(n)`, `su(n)`,
 * `le(n)`, `ue(v)`, `se(v)`, `te(v)`, `uvlc()`, `leb128()`. A descriptor that cannot be rebuilt (`ns(n)`, strings,
 * byte arrays) prints `--` in the bits column. SPEC-ASSUMPTION(#822): this rule, and the `--` for ns(n).
 *
 * @param syntax_node_fb A finished ovt.fb.SyntaxNode FlatBuffer as ovt_stream_unit_syntax() returns it (no file
 *                       identifier, not size-prefixed); owned by the caller, only read during the call.
 * @param len            Its size in bytes. A zero length gives an empty text.
 * @param out_text       Receives the UTF-8 text, NUL-terminated past `size`; owned by the caller and released with
 *                       ovt_buffer_free() (chapter 12 §12.3.1 rule 3a). Zeroed when the call fails.
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL pointer; OVT_E_CORRUPT when the buffer does not verify as a
 *         SyntaxNode; OVT_E_NOMEM; OVT_E_INTERNAL.
 */
OVT_API ovt_status ovt_syntax_trace(const uint8_t* syntax_node_fb, size_t len, ovt_Buffer* out_text);

#ifdef __cplusplus
}
#endif

#endif /* OVT_SYNTAX_H */
