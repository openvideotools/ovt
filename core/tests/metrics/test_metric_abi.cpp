// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-3.300 step 2: ovt/abi/metric_v1.h compiles as C++20 (this file) and as C11 (abi_header_c.c), and its public
// layouts are pinned (chapter 12 §12.3.1 rule 7: fields are only appended, so these offsets never change).

#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "ovt/abi/metric_v1.h"
#include "ovt/color.h"
#include "ovt/decode.h"
#include "ovt/metrics.h"
#include "ovt/status.h"

extern "C" int ovt_test_metric_abi_header_c(void);

static_assert(OVT_METRIC_ABI_VERSION == 1);
static_assert(offsetof(ovt_MetricApi, struct_size) == 0);
static_assert(offsetof(ovt_MetricApi, abi_version) == 4);
static_assert(offsetof(ovt_MetricPlanes, struct_size) == 0);
static_assert(sizeof(ovt_ColorDescription) == 44, "backbone §0.5.6");
static_assert(sizeof(ovt_FrameId) == 8);
static_assert(sizeof(ovt_Buffer) == 2 * sizeof(void*));
#if UINTPTR_MAX == UINT64_MAX
static_assert(offsetof(ovt_MetricPlanes, data) == 8);
static_assert(offsetof(ovt_MetricPlanes, stride_bytes) == 32);
static_assert(offsetof(ovt_MetricPlanes, width) == 44);
static_assert(offsetof(ovt_MetricPlanes, height) == 56);
static_assert(offsetof(ovt_MetricPlanes, planes) == 68);
static_assert(offsetof(ovt_MetricPlanes, domain) == 72);
static_assert(offsetof(ovt_MetricPlanes, color) == 76);
static_assert(offsetof(ovt_MetricPlanes, display_peak_nits) == 120);
static_assert(sizeof(ovt_MetricPlanes) == 128);
static_assert(offsetof(ovt_MetricApi, descriptor) == 8);
static_assert(offsetof(ovt_MetricApi, finish) == 40);
static_assert(sizeof(ovt_MetricApi) == 48);
#endif
// The repeated types are the same types (one definition survives the shared guards).
static_assert(std::is_same_v<decltype(ovt_MetricPlanes::color), ovt_ColorDescription>);

TEST_CASE("ovt/abi/metric_v1.h compiles as C11 and C++20 with the pinned layout", "[metrics][abi][fast]") {
  // The static_asserts above and the C translation unit are the test; both headers were included together.
  CHECK(ovt_test_metric_abi_header_c() ==
        static_cast<int>(sizeof(ovt_MetricPlanes)) + static_cast<int>(sizeof(ovt_MetricApi)) + 1);
  ovt_MetricPlanes planes{};
  planes.struct_size = sizeof(planes);
  CHECK(planes.data[1] == nullptr);
  CHECK(planes.color.struct_size == 0);
  CHECK(planes.domain == OVT_METRIC_DOMAIN_CODE_VALUE);
  ovt_MetricApi api{};
  CHECK(api.descriptor == nullptr);
}
