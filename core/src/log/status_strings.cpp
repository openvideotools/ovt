// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_status_string (chapter 12 §12.3.3). Moved here from core/src/status.cpp by WI-1.512, which owns the
// ovt/status.h strings; module code and log lines use it through the module objects.

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

}  // extern "C"
