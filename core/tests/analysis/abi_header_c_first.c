// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Compiled as C11: the ABI header first, then ovt/status.h. Both define ovt_status and ovt_Buffer under one
// guard (SPEC-ASSUMPTION(#211)), so this order must compile, and the header must be self-contained C.

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/status.h"

_Static_assert(sizeof(ovt_Buffer) == 2 * sizeof(void*), "ovt_Buffer is {data, size}");
_Static_assert(OVT_E_INTERNAL == 8, "ovt_status values are fixed");

int ovt_test_abi_header_c_first(void);
int ovt_test_abi_header_c_first(void) {
  ovt_AnalysisCaps caps = {0};
  caps.struct_size = (uint32_t)sizeof(caps);
  return (int)caps.struct_size + (int)OVT_ANALYSIS_ABI_VERSION;
}
