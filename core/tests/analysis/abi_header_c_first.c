// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// C11: the ABI header (self-contained) first, then ovt/status.h with the same guarded types (#211).

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/status.h"

int ovt_test_abi_header_c_first(void);
int ovt_test_abi_header_c_first(void) {
  ovt_AnalysisCaps caps = {0};
  caps.struct_size = (uint32_t)sizeof(caps);
  return (int)caps.struct_size + (int)OVT_ANALYSIS_ABI_VERSION;
}
