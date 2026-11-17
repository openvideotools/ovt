// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/index.h
 * @brief Stream C API: one video track of a container, its StreamIndex and the lookups on it
 *        (chapter 12 §12.3.8, chapter 07 §7.4).
 *
 * WI-1.121 declares the whole `ovt_Stream` block of §12.3.8. ovt_stream_open(), ovt_stream_codec(),
 * ovt_stream_build_index(), ovt_stream_index(), ovt_stream_rap_before() and ovt_stream_close() are implemented;
 * WI-1.125 adds ovt_stream_set_index_flags() / OVT_INDEX_ALLOW_PARTIAL, the job wiring of ovt_stream_build_index()
 * and ovt_stream_unit_syntax(). Every other function returns OVT_E_UNSUPPORTED (the int getters -1) until the work
 * item that owns it lands (WI-1.130 and later). The header text is finalised by WI-1.511.
 *
 * Ownership (chapter 12 §12.3.1 rule 3): an ovt_Buffer returned through an out-parameter is owned by the
 * caller and released with ovt_buffer_free(); ovt_stream_index() is the single by-value return and its buffer
 * is owned by the handle.
 */
#ifndef OVT_INDEX_H
#define OVT_INDEX_H

#include <stddef.h> /* NOLINT(modernize-deprecated-headers): a C11 header */
#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#include "ovt/color.h"
#include "ovt/container.h"
#include "ovt/jobs.h"
#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque stream handle. Created by ovt_stream_open(), released with ovt_stream_close(). */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_Stream ovt_Stream;

/**
 * Opens video track @p track_index of @p container. On success *@p out is owned by the caller. The formats with a
 * unit map today are "annexb" (H.264, H.265), "obu" and "ivf" (AV1), each with the single track 0; any other
 * format returns OVT_E_UNSUPPORTED. @p container must outlive the stream.
 */
OVT_API ovt_status ovt_stream_open(ovt_Container* container, int track_index, ovt_Stream** out);
/** The stream's codec as an ovt.fb.Codec value; 0 (Unknown) for NULL. */
OVT_API uint8_t ovt_stream_codec(const ovt_Stream* stream);
/**
 * Options of ovt_stream_build_index() (chapter 07 §7.4.6), set per stream with ovt_stream_set_index_flags().
 * SPEC-ASSUMPTION(#710): §7.4.6 places OVT_INDEX_ALLOW_PARTIAL on the call's "job options", which ovt/jobs.h does not
 * have; the flag is a stream option so that the call keeps its §0.6.1 signature.
 */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef enum ovt_IndexFlags {
  /**
   * A build cancelled through its ovt_Job returns OVT_OK with the index of the frames that were complete at that
   * point, carrying Warning SYN-GEN-0103 "index truncated at unit N" (N = the first unit left out); without the flag
   * it returns OVT_E_CANCELLED and no index. The next ovt_stream_build_index() on the stream resumes from that unit
   * and returns the whole index, byte-identical to an uninterrupted build.
   */
  OVT_INDEX_ALLOW_PARTIAL = 1
} ovt_IndexFlags;

/** Sets the ovt_IndexFlags of @p stream for its next ovt_stream_build_index() calls (default 0). OVT_E_INVALID_ARG
 *  for a NULL stream or a bit that is not an ovt_IndexFlags value. */
OVT_API ovt_status ovt_stream_set_index_flags(ovt_Stream* stream, uint32_t flags);
/**
 * Builds the StreamIndex (chapter 07 §7.4.2) into *@p out_index (caller-owned) and keeps a copy for
 * ovt_stream_index(). A damaged or truncated stream still returns OVT_OK: the findings are Diagnostics of the
 * index. The cache is neither read nor written yet.
 *
 * @p job (may be NULL) reports progress as bytes of the file (stage "index") and is checked for cancellation at
 * least every 256 units and every 50 ms (chapter 07 §7.4.6): a cancelled build returns OVT_E_CANCELLED and leaves
 * the stream usable, or the partial index when OVT_INDEX_ALLOW_PARTIAL is set. Slice headers of frames beyond
 * the first 10 000 are indexed without a header tree (§7.4.5); ovt_stream_unit_syntax() rebuilds one on demand.
 */
OVT_API ovt_status ovt_stream_build_index(ovt_Stream* stream, ovt_Job* job, ovt_Buffer* out_index);
/** The last built StreamIndex. Owned by the handle (rule 3b), valid until the next ovt_stream_build_index() or
 *  ovt_stream_close(); .data == NULL before ovt_stream_build_index(). */
OVT_API ovt_Buffer ovt_stream_index(ovt_Stream* stream);
/** IndexSummary of the built index in *@p out (caller-owned). */
OVT_API ovt_status ovt_stream_index_summary(ovt_Stream* stream, ovt_Buffer* out);
/** The bytes of unit @p unit_index as stored (@p out_raw) and without emulation prevention (@p out_rbsp); either
 *  may be NULL. */
OVT_API ovt_status ovt_stream_unit_bytes(ovt_Stream* stream, uint32_t unit_index, ovt_Buffer* out_raw,
                                         ovt_Buffer* out_rbsp);
/**
 * SyntaxNode of unit @p unit_index in *@p out (caller-owned, root table ovt.fb.SyntaxNode, no file identifier);
 * re-parses lazy trees (chapter 07 §7.4.5). Every unit is re-parsed from the file with the parameter sets and
 * earlier units it depends on, so the tree is byte-identical to the one an eager index records for the same unit.
 * A unit that records nothing (an unparsed unit without diagnostics) gives an empty buffer with OVT_OK.
 * OVT_E_INVALID_ARG before ovt_stream_build_index() or for an index at or beyond the unit count.
 */
OVT_API ovt_status ovt_stream_unit_syntax(ovt_Stream* stream, uint32_t unit_index, ovt_Buffer* out);
/** FrameInfo of frame @p decode_index in *@p out. */
OVT_API ovt_status ovt_stream_frame_info(ovt_Stream* stream, int decode_index, ovt_Buffer* out);
/** Number of frames of the built index; -1 before ovt_stream_build_index(). */
OVT_API int ovt_stream_frame_count(const ovt_Stream* stream);
/** Decode index of the frame shown at @p display_index; -1 if invalid. */
OVT_API int ovt_stream_display_to_decode(const ovt_Stream* stream, int display_index);
/** Display index of frame @p decode_index; -1 if hidden. */
OVT_API int ovt_stream_decode_to_display(const ovt_Stream* stream, int decode_index);
/** Decode index of the nearest random access point at or before @p decode_index; -1 when there is none, the index
 *  is not built or @p decode_index is out of range. */
OVT_API int ovt_stream_rap_before(const ovt_Stream* stream, int decode_index);
/** Merged container and bitstream colour description; conflicts are logged. */
OVT_API ovt_status ovt_stream_color(ovt_Stream* stream, ovt_ColorDescription* out);
/** The units @p unit_indices re-framed as @p to_format ("annexb", "obu", "ivf") in *@p out_bytes. */
OVT_API ovt_status ovt_stream_extract(ovt_Stream* stream, const uint32_t* unit_indices, size_t count,
                                      const char* to_format, ovt_Buffer* out_bytes);
/** UIntList of the unit indices of the smallest decodable sequence that contains frame @p decode_index. */
OVT_API ovt_status ovt_stream_min_cvs_units(ovt_Stream* stream, int decode_index, int with_sei, ovt_Buffer* out);
/** Releases the stream. A NULL pointer is ignored. */
OVT_API void ovt_stream_close(ovt_Stream* stream);

#ifdef __cplusplus
}
#endif

#endif /* OVT_INDEX_H */
