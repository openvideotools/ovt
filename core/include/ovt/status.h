// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/status.h
 * @brief Status codes, caller-owned buffers and the per-thread last error (chapter 12 §12.3.3).
 *
 * Stub created by WI-0.000: the declarations are final; ovt_last_error() returns an empty buffer until
 * the Diagnostic FlatBuffer exists (chapter 06).
 */
#ifndef OVT_STATUS_H
#define OVT_STATUS_H

#include <stddef.h>
#include <stdint.h>

/** Marks a function exported from libovt_core (chapter 12 §12.3.2: nothing else is exported). */
/* clang-format off */
#ifndef OVT_API
#  if defined(__GNUC__) || defined(__clang__)
#    define OVT_API __attribute__((visibility("default")))
#  else
#    define OVT_API
#  endif
#endif
/* clang-format on */

#ifdef __cplusplus
extern "C" {
#endif

/* ovt/abi/analysis_decoder_v1.h repeats these two types under the same guard. SPEC-ASSUMPTION(#211). */
#ifndef OVT_STATUS_TYPES_DEFINED
#define OVT_STATUS_TYPES_DEFINED
/* NOLINTBEGIN(modernize-use-using): a C11 header */
/** Result of every fallible C API call. Values are never renumbered; new values are appended. */
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
/* NOLINTEND(modernize-use-using) */
#endif /* OVT_STATUS_TYPES_DEFINED */

/**
 * Returns the enumerator name of @p status, for example "OVT_E_CORRUPT"; "OVT_E_UNKNOWN" for a value
 * outside the enum. Never fails. The returned string is a static constant owned by the library and
 * MUST NOT be freed.
 */
OVT_API const char* ovt_status_string(ovt_status status);

/**
 * Releases a caller-owned buffer (one returned through an out-parameter, rule 12.3.1.3a) and zeroes
 * @p buf. A no-op on a zeroed buffer or a NULL pointer. MUST NOT be called on a buffer returned by
 * value, which the library owns.
 */
OVT_API void ovt_buffer_free(ovt_Buffer* buf);

/**
 * Last error detail for the calling thread as a Diagnostic FlatBuffer, cleared by every API call.
 * By-value return: owned by the library (rule 12.3.1.3b), valid until the next API call on this
 * thread; MUST NOT be freed.
 */
OVT_API ovt_Buffer ovt_last_error(void);

/**
 * The same last error as JSON, or "null" when the last call on this thread succeeded. Owned by the
 * library, valid until the next API call on this thread; MUST NOT be freed.
 */
OVT_API const char* ovt_last_error_json(void);

#ifdef __cplusplus
}
#endif

#endif /* OVT_STATUS_H */
