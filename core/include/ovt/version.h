// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * @file ovt/version.h
 * @brief API version of the headers and runtime version of the loaded library (chapter 12 §12.3.2).
 */
#ifndef OVT_VERSION_H
#define OVT_VERSION_H

#include <stdint.h>

#include "ovt/status.h"

// ovt_version*() report the product version (VERSION file); OVT_API_VERSION_* is the independent C API
// version (1.0.0) and sets the SONAME (chapter 12 §12.3.2).

/** Version of the C API these headers describe (semantic versioning, chapter 12 §12.3.2 policy). */
#define OVT_API_VERSION_MAJOR 1
#define OVT_API_VERSION_MINOR 0
#define OVT_API_VERSION_PATCH 0
#define OVT_API_VERSION ((OVT_API_VERSION_MAJOR << 16) | (OVT_API_VERSION_MINOR << 8) | OVT_API_VERSION_PATCH)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Writes the product version of the loaded library (the VERSION file, e.g. 0.1.0). Never fails. Any
 * out-pointer may be NULL, in which case that component is not written.
 */
OVT_API void ovt_version(int* out_major, int* out_minor, int* out_patch);

/**
 * The product version of the loaded library as "<major>.<minor>.<patch>[<suffix>]", without the git
 * sha (e.g. "0.1.0"). Never fails. Static string owned by the library; MUST NOT be freed.
 */
OVT_API const char* ovt_version_string(void);

/**
 * Build information as a JSON object:
 * {"commit":"<sha>","profile":"<debug|release|...>","platform":"<os>-<arch>","features":[...]}.
 * Never fails. Static string owned by the library; MUST NOT be freed.
 */
OVT_API const char* ovt_build_info_json(void);

/**
 * Returns OVT_OK if the running library is ABI-compatible with @p header_api_version (pass
 * OVT_API_VERSION): same major and a minor not newer than the library's. Otherwise returns
 * OVT_E_UNSUPPORTED.
 */
OVT_API ovt_status ovt_check_version(uint32_t header_api_version);

#ifdef __cplusplus
}
#endif

#endif /* OVT_VERSION_H */
