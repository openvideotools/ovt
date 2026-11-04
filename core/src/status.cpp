// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/status.h entry points (chapter 12 §12.3.3). The per-thread Diagnostic arrives with the model
// module; until then the last error is always empty.

#include "ovt/status.h"

extern "C" {

OVT_API const char* ovt_status_string(ovt_status status) {
  switch (status) {
    case OVT_OK:
      return "OVT_OK";
    case OVT_E_INVALID_ARG:
      return "OVT_E_INVALID_ARG";
    case OVT_E_IO:
      return "OVT_E_IO";
    case OVT_E_UNSUPPORTED:
      return "OVT_E_UNSUPPORTED";
    case OVT_E_CORRUPT:
      return "OVT_E_CORRUPT";
    case OVT_E_NOMEM:
      return "OVT_E_NOMEM";
    case OVT_E_CANCELLED:
      return "OVT_E_CANCELLED";
    case OVT_E_PLUGIN:
      return "OVT_E_PLUGIN";
    case OVT_E_INTERNAL:
      return "OVT_E_INTERNAL";
  }
  return "OVT_E_UNKNOWN";
}

OVT_API void ovt_buffer_free(ovt_Buffer* buf) {
  if (buf == nullptr) {
    return;
  }
  // No API function hands out a caller-owned buffer yet; once one does, its allocator is released here.
  buf->data = nullptr;
  buf->size = 0;
}

OVT_API ovt_Buffer ovt_last_error(void) { return ovt_Buffer{nullptr, 0}; }

OVT_API const char* ovt_last_error_json(void) { return "null"; }

}  // extern "C"
