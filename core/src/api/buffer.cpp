// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::api::make_buffer (core/src/api/buffer.hpp): the allocator of every caller-owned ovt_Buffer (chapter 12
// §12.3.1 rule 3a). ovt_buffer_free() in core/src/status.cpp releases with delete[]; keep the two in step.

#include "api/buffer.hpp"

#include <cstring>
#include <new>

namespace ovt::api {

ovt_status make_buffer(const void* data, std::size_t size, ovt_Buffer* out) noexcept {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  out->data = nullptr;
  out->size = 0;
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the C ABI allocator (D4), released by delete[]
  auto* mem = new (std::nothrow) uint8_t[size + 1];
  if (mem == nullptr) {
    return OVT_E_NOMEM;
  }
  if (size > 0 && data != nullptr) {
    std::memcpy(mem, data, size);
  }
  mem[size] = 0;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): the terminator slot
  out->data = mem;
  out->size = size;
  return OVT_OK;
}

}  // namespace ovt::api
