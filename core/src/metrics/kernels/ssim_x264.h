// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `ssim_x264` metric (chapter 9 §9.4.4, WI-3.308): an in-tree ovt_metric_v1 implementation.

#ifndef OVT_SRC_METRICS_KERNELS_SSIM_X264_H
#define OVT_SRC_METRICS_KERNELS_SSIM_X264_H

#include "ovt/abi/metric_v1.h"

namespace ovt::metrics {

// The function table of the ssim_x264 metric; static, never null. Registered by register_builtin_kernels().
[[nodiscard]] const ovt_MetricApi* ssim_x264_metric_api() noexcept;

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_KERNELS_SSIM_X264_H
