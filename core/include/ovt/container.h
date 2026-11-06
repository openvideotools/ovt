// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/container.h
 * @brief Container module C API: open a file, sniff its format, read the box tree and the byte service
 *        (chapter 12 §12.3.7, chapter 08 §8.8).
 *
 * WI-1.200 declares the whole §12.3.7 symbol set; every function except ovt_container_open(),
 * ovt_container_open_format(), ovt_container_format(), ovt_container_tree(), ovt_container_track_count(),
 * ovt_container_size() and ovt_container_close() returns OVT_E_UNSUPPORTED until the work item that owns it
 * lands (WI-1.201 onwards, WI-1.234). The header text is finalised by WI-1.511.
 *
 * Ownership (chapter 12 §12.3.1 rule 3): an ovt_Buffer returned through an out-parameter is owned by the
 * caller and released with ovt_buffer_free(); ovt_container_tree() is the single by-value return and its
 * buffer is owned by the handle.
 */
#ifndef OVT_CONTAINER_H
#define OVT_CONTAINER_H

#include <stdint.h> /* NOLINT(modernize-deprecated-headers): a C11 header */

#include "ovt/jobs.h"
#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque file handle (chapter 12 §12.3.6). Repeating a typedef is legal in C11 and C++. */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_File ovt_File;
/** Colour description struct (chapter 00 §0.5.6); only its address crosses this header. */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_ColorDescription ovt_ColorDescription;
/** Opaque container handle. Created by ovt_container_open*(), released with ovt_container_close(). */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_Container ovt_Container;

/**
 * Sniffs the format of @p file (chapter 08 §8.6.1) and opens it. On success *@p out is owned by the caller.
 * Returns OVT_E_UNSUPPORTED when the format is unknown or not yet readable, and OVT_E_INVALID_ARG for a NULL
 * argument or for a headerless raw file (use ovt_container_open_format()). @p file must outlive the container.
 */
OVT_API ovt_status ovt_container_open(ovt_File* file, ovt_Container** out);

/**
 * Opens @p file as @p format ("isobmff", "ivf", "annexb", "obu", "y4m", "raw"; phase 4: "ts", "mkv") without
 * sniffing. @p raw_format ("WxH:pix_fmt[:num/den]") is required for "raw" and ignored otherwise; @p codec_hint
 * ("h264", "h265", "av1") may be NULL. Strings are borrowed for the call.
 */
OVT_API ovt_status ovt_container_open_format(ovt_File* file, const char* format, const char* raw_format,
                                             const char* codec_hint, ovt_Container** out);

/** Phase 4 (WI-4.210): DASH MPD / HLS playlist. Returns OVT_E_UNSUPPORTED. */
OVT_API ovt_status ovt_container_open_manifest(const char* url_or_path, const char* variant_selector,
                                               ovt_Container** out);

/** The format name ("isobmff", "ivf", "annexb", "obu", "y4m", "raw"). Static string, never NULL for a valid handle. */
OVT_API const char* ovt_container_format(const ovt_Container* container);

/**
 * The ContainerTree FlatBuffer. By-value return: owned by the handle, valid until the next expand or close;
 * MUST NOT be freed. Unexpanded boxes carry CNT-0007 (chapter 08 §8.5).
 */
OVT_API ovt_Buffer ovt_container_tree(ovt_Container* container);

/** Decodes the fields and children of the box at @p box_offset. Invalidates the tree buffer. */
OVT_API ovt_status ovt_container_expand(ovt_Container* container, uint64_t box_offset);
/** Expands everything except mdat payloads. @p job may be NULL. */
OVT_API ovt_status ovt_container_expand_all(ovt_Container* container, ovt_Job* job);
/** Number of tracks (0 until the readers land). */
OVT_API int ovt_container_track_count(const ovt_Container* container);
/** TrackInfo FlatBuffer of one track; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_track_info(ovt_Container* container, int track_index, ovt_Buffer* out);
/** SampleTable FlatBuffer; @p count 0 = all; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_sample_table(ovt_Container* container, int track_index, uint32_t first, uint32_t count,
                                              ovt_Buffer* out);
/** FragmentTimeline FlatBuffer; @p job may be NULL; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_fragments(ovt_Container* container, int track_index, ovt_Job* job, ovt_Buffer* out);
/** EncryptionInfo FlatBuffer; @p count 0 = all; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_encryption(ovt_Container* container, int track_index, uint64_t first, uint64_t count,
                                            ovt_Buffer* out);
/** HdrMetadata FlatBuffer; @p job and @p out_color may be NULL; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_hdr_metadata(ovt_Container* container, int track_index, ovt_Job* job, ovt_Buffer* out,
                                              ovt_ColorDescription* out_color);
/** Phase 4 (WI-4.213): HEIF/AVIF items as ItemInfoList; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_items(ovt_Container* container, ovt_Buffer* out);
/** Phase 4: the bytes of one HEIF/AVIF item; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_item_bytes(ovt_Container* container, uint32_t item_id, ovt_Buffer* out);
/** Validation findings as a DiagnosticList; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_validate(ovt_Container* container, ovt_Buffer* out);
/** The CNT-* registry of chapter 08 §8.5; no handle needed; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_list_diagnostics(ovt_Buffer* out);

/** Byte service (chapter 08 §8.8): copies up to @p len (at most 1 MiB) bytes at @p offset; *@p out is caller-owned. */
OVT_API ovt_status ovt_container_bytes(ovt_Container* container, uint64_t offset, uint32_t len, ovt_Buffer* out);
/** Maps a file offset to the innermost box containing it. */
OVT_API ovt_status ovt_container_box_at(ovt_Container* container, uint64_t offset, uint64_t* out_box_offset,
                                        int* out_depth);
/** Which segment a logical offset belongs to; JSON in *@p out_json (caller-owned). */
OVT_API ovt_status ovt_container_segment_for_offset(ovt_Container* container, uint64_t offset, ovt_Buffer* out_json);
/** File size in bytes; 0 for a NULL handle. Never fails. */
OVT_API uint64_t ovt_container_size(const ovt_Container* container);
/** Page-cache and parse statistics as JSON in *@p out_json (caller-owned). */
OVT_API ovt_status ovt_container_stats_json(ovt_Container* container, ovt_Buffer* out_json);

/** Releases the container. A NULL pointer is ignored. Only after every stream opened on it is closed. */
OVT_API void ovt_container_close(ovt_Container* container);

#ifdef __cplusplus
}
#endif

#endif /* OVT_CONTAINER_H */
