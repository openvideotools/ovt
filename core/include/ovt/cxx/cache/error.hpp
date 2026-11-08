// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The error type of the cache module (chapter 06 §6.7, WI-0.091). C++ helper header (D22): semi-public, no
// ABI promise, never included from a C header.

#ifndef OVT_CXX_CACHE_ERROR_HPP
#define OVT_CXX_CACHE_ERROR_HPP

#include <stdexcept>
#include <string>
#include <utility>

#include "ovt/status.h"

namespace ovt::cache {

/// Thrown by every cache operation that fails; the C API maps it to its `code` (§12.3.1 rule 8).
/// OVT_E_IO: the file system refused; OVT_E_CORRUPT: a file in the cache is malformed; OVT_E_INVALID_ARG: the
/// caller's buffer or option is wrong; OVT_E_UNSUPPORTED: a write on a read-only cache.
struct CacheError : std::runtime_error {
  ovt_status code;
  CacheError(ovt_status c, std::string msg) : std::runtime_error(std::move(msg)), code(c) {}
};

}  // namespace ovt::cache

#endif  // OVT_CXX_CACHE_ERROR_HPP
