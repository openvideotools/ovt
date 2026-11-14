// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `ssim` metric (chapter 9 §9.4.3, §9.4.7; WI-3.307): an in-tree ovt_metric_v1 implementation.

#ifndef OVT_SRC_METRICS_KERNELS_SSIM_METRIC_H
#define OVT_SRC_METRICS_KERNELS_SSIM_METRIC_H

#include "ovt/abi/metric_v1.h"

namespace ovt::metrics {

// The function table of the ssim metric; static, never null. Registered by register_builtin_kernels().
[[nodiscard]] const ovt_MetricApi* ssim_metric_api() noexcept;

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_KERNELS_SSIM_METRIC_H
