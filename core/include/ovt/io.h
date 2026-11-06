// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/io.h
 * @brief Read-only input files (chapter 12 §12.3.6, chapter 7 §7.1, WI-1.101).
 *
 * An ovt_File wraps one ovt::io::RandomAccessFile. It is opened by path on desktop systems and by file
 * descriptor on Android, where the shell resolves a content:// URI with ContentResolver.openFileDescriptor()
 * and passes ParcelFileDescriptor.detachFd(). Both open functions read with pread(2) (chapter 8 §8.1 rule 5:
 * the container byte statistics must stay exact).
 */
#ifndef OVT_IO_H
#define OVT_IO_H

#include <stddef.h>
#include <stdint.h>

#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** An open input file. Opaque; released with ovt_file_close(). */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef struct ovt_File ovt_File;

/**
 * Opens the regular file at @p path (UTF-8, read-only). On success *@p out receives a handle the caller
 * releases with ovt_file_close().
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL or empty @p path, a NULL @p out, a directory, socket or FIFO,
 *         or an empty file; OVT_E_IO when the operating system refuses the open or a read. *@p out is set to
 *         NULL on failure.
 */
OVT_API ovt_status ovt_file_open(const char* path, ovt_File** out);

/**
 * Opens an already-open, readable descriptor @p fd.
 *
 * @p size is the total length in bytes (Android: ParcelFileDescriptor.getStatSize()). 0 or UINT64_MAX means
 * unknown: the size then comes from fstat(2), which needs a non-empty regular file. A pipe has no size, so
 * the caller copies such content to a file first.
 *
 * @p take_ownership = 1: the ovt_File adopts @p fd and closes it in ovt_file_close(). Ownership passes at the
 * call, so @p fd is also closed when this function fails. @p take_ownership = 0: the library duplicates
 * @p fd and the caller keeps (and closes) the original.
 *
 * @p display_name (nullable, UTF-8) is what ovt_file_display_name() returns; NULL or "" gives "fd:<fd>".
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for @p fd < 0, a NULL @p out, a directory, a descriptor not open for
 *         reading, or an unknown @p size that fstat(2) cannot supply (not a regular file, or empty);
 *         OVT_E_IO for a closed descriptor or any other operating-system failure. *@p out is set to NULL on
 *         failure.
 */
OVT_API ovt_status ovt_file_open_fd(int fd, uint64_t size, int take_ownership, const char* display_name,
                                    ovt_File** out);

/** Size of the file in bytes; 0 for a NULL @p file. */
OVT_API uint64_t ovt_file_size(const ovt_File* file);

/**
 * Copies up to @p len bytes starting at @p offset into @p dst. *@p out_read (nullable) receives the number of
 * bytes copied, which is less than @p len at the end of the file and 0 at or after it (not an error).
 * Thread-safe: may be called concurrently on the same handle.
 *
 * @return OVT_OK; OVT_E_INVALID_ARG for a NULL @p file, a NULL @p dst with @p len > 0, or @p len > 2^31;
 *         OVT_E_IO when the operating system reports a read error.
 */
OVT_API ovt_status ovt_file_read(ovt_File* file, uint64_t offset, uint8_t* dst, size_t len, size_t* out_read);

/**
 * The path given to ovt_file_open(), or the display name given to ovt_file_open_fd() ("fd:<n>" by default).
 * Owned by the handle and valid until ovt_file_close(); NULL for a NULL @p file.
 */
OVT_API const char* ovt_file_display_name(const ovt_File* file);

/** Closes @p file (NULL is a no-op). Call it only after every container opened on the file is closed. */
OVT_API void ovt_file_close(ovt_File* file);

#ifdef __cplusplus
}
#endif

#endif /* OVT_IO_H */
