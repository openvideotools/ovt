// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/log.h
 * @brief Routing of library log lines to the host application (chapter 12 §12.3.4, FR-API-006).
 *
 * The library never writes to stdout or stderr by itself: until ovt_set_logger() installs a callback,
 * every log line is dropped (chapter 04 FR-API-006).
 */
#ifndef OVT_LOG_H
#define OVT_LOG_H

#include "ovt/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Log levels of ovt_log_fn and ovt_set_log_level(). */
#define OVT_LOG_TRACE 0
#define OVT_LOG_DEBUG 1
#define OVT_LOG_INFO 2
#define OVT_LOG_WARN 3
#define OVT_LOG_ERROR 4
#define OVT_LOG_FATAL 5
/** Threshold that disables every log line (the CLI's `--log-level off`, chapter 12 §12.1.2). */
#define OVT_LOG_OFF 6

/**
 * Receives one log line. @p level is 0 trace, 1 debug, 2 info, 3 warn, 4 error, 5 fatal. @p module (for
 * example "io") and @p msg are NUL-terminated UTF-8 strings owned by the library, valid only for the
 * duration of the call; a diagnostic code, when the line has one, starts @p msg as "[CODE] ". @p user is
 * the pointer given to ovt_set_logger(). The callback MAY be invoked from any thread, concurrently, and
 * MUST NOT call back into OVT or throw (chapter 12 §12.3.1 rules 4 and 8).
 */
/* NOLINTNEXTLINE(modernize-use-using): a C11 header */
typedef void (*ovt_log_fn)(int level, const char* module, const char* msg, void* user);

/**
 * Installs @p fn as the logger, with @p user passed back on every call (the library does not own
 * @p user). NULL restores the default logger, which drops every line (chapter 04 FR-API-006). MUST be
 * called before any other API call from any thread (chapter 12 §12.3.1 rule 4). A line already being
 * delivered on another thread may still reach the previous logger after this returns, so keep the previous
 * @p user valid until no other thread can be logging. Never fails.
 */
OVT_API void ovt_set_logger(ovt_log_fn fn, void* user);

/**
 * Sets the threshold: lines below @p level are not formatted at all. Values below 0 are treated as 0
 * and above OVT_LOG_OFF as OVT_LOG_OFF. The default is OVT_LOG_WARN. Thread-safe; never fails.
 */
OVT_API void ovt_set_log_level(int level);

#ifdef __cplusplus
}
#endif

#endif /* OVT_LOG_H */
