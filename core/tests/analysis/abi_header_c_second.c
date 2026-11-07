// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
// C11: ovt/status.h, then the ABI header, then ovt/ovt.h (the other order of abi_header_c_first.c).

// clang-format off
#include "ovt/status.h"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/ovt.h"
// clang-format on

int ovt_test_abi_header_c_second(void);
int ovt_test_abi_header_c_second(void) {
  ovt_AnalysisDecoderApi api = {0};
  api.struct_size = (uint32_t)sizeof(api);
  api.abi_version = OVT_ANALYSIS_ABI_VERSION;
  return (int)api.abi_version + (int)(OVT_GRID_FILTER_IDX >> 15);
}
