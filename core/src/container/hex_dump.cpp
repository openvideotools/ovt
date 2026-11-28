// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/hex_dump.h"

#include "container/container.hpp"
#include "ovt/container.h"

namespace ovt::container {

namespace {

// Releases a buffer made by api::make_buffer: the delete[] of ovt_buffer_free() (core/src/status.cpp, which lives in
// the shared library and so is not linked into the module objects).
void release(ovt_Buffer& buf) noexcept {
  delete[] buf.data;  // NOLINT(cppcoreguidelines-owning-memory): the allocator of rule 3a is new[] (api/buffer.cpp)
  buf = ovt_Buffer{nullptr, 0};
}

}  // namespace

ovt_status hex_dump_bytes(ovt_Container* container, uint64_t offset, uint32_t len, std::string* out) noexcept {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  out->clear();
  ovt_Buffer buf{nullptr, 0};
  const ovt_status st = ovt_container_bytes(container, offset, len, &buf);
  if (st != OVT_OK) {
    return st;
  }
  try {
    *out = format_hex_dump(offset, std::span<const uint8_t>(buf.data, buf.size));
  } catch (...) {
    release(buf);
    return status_of_current_exception();
  }
  release(buf);
  return OVT_OK;
}

}  // namespace ovt::container
