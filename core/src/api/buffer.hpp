// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Caller-owned ovt_Buffer allocation (chapter 12 §12.3.1 rule 3a, D4): every buffer an entry point hands out
// through an out-parameter comes from here and is released by ovt_buffer_free() (core/src/status.cpp), so the
// two sides always agree on the allocator. Internal header (not installed). WI-3.300 adds it for
// ovt_metrics_list_descriptors(); later entry points reuse it.

#ifndef OVT_SRC_API_BUFFER_HPP
#define OVT_SRC_API_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ovt/status.h"

namespace ovt::api {

// Copies `size` bytes into a new caller-owned buffer. The allocation holds one extra zero byte after the data
// (not counted in size), so text buffers are NUL-terminated. OVT_E_NOMEM when the allocation fails; *out is then
// zeroed. noexcept.
ovt_status make_buffer(const void* data, std::size_t size, ovt_Buffer* out) noexcept;

inline ovt_status make_buffer(std::string_view text, ovt_Buffer* out) noexcept {
  return make_buffer(text.data(), text.size(), out);
}

}  // namespace ovt::api

#endif  // OVT_SRC_API_BUFFER_HPP
