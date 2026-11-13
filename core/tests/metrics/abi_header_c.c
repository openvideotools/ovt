// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
// C11: ovt/abi/metric_v1.h compiles self-contained and first (WI-3.300 step 2), then with every other public
// header through ovt/ovt.h, so the shared-guard copies of ovt_status, ovt_FrameId and ovt_ColorDescription
// coexist (#156, #211).

#include "ovt/abi/metric_v1.h"
#include "ovt/ovt.h"

#include <stddef.h>

_Static_assert(offsetof(ovt_MetricApi, struct_size) == 0, "chapter 12 rule 7");
_Static_assert(offsetof(ovt_MetricPlanes, struct_size) == 0, "chapter 12 rule 7");
_Static_assert(sizeof(ovt_ColorDescription) == 44, "backbone 0.5.6");
_Static_assert(sizeof(ovt_FrameId) == 8, "backbone 0.5.3");
_Static_assert(OVT_METRIC_ABI_VERSION == 1, "v1 header");

int ovt_test_metric_abi_header_c(void);
int ovt_test_metric_abi_header_c(void) {
  ovt_MetricPlanes planes = {0};
  ovt_MetricApi api = {0};
  planes.struct_size = (uint32_t)sizeof(planes);
  planes.domain = OVT_METRIC_DOMAIN_CODE_VALUE;
  api.struct_size = (uint32_t)sizeof(api);
  return (int)planes.struct_size + (int)api.struct_size + (int)OVT_METRIC_ABI_VERSION;
}
