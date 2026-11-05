// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/status.h entry points (chapter 12 §12.3.3) except ovt_status_string, which lives in
// core/src/log/status_strings.cpp (WI-1.512). The per-thread Diagnostic arrives with the model module; until
// then the last error is always empty.

#include "ovt/status.h"

extern "C" {

OVT_API void ovt_buffer_free(ovt_Buffer* buf) {
  if (buf == nullptr) {
    return;
  }
  // No API function hands out a caller-owned buffer yet; once one does, its allocator is released here.
  buf->data = nullptr;
  buf->size = 0;
}

OVT_API ovt_Buffer ovt_last_error(void) {
  return ovt_Buffer{nullptr, 0};
}

OVT_API const char* ovt_last_error_json(void) {
  return "null";
}

}  // extern "C"
