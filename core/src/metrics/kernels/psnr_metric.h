// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `psnr` metric (chapter 9 §9.4.2, §9.4.7; WI-3.306): an in-tree ovt_metric_v1 implementation.

#ifndef OVT_SRC_METRICS_KERNELS_PSNR_METRIC_H
#define OVT_SRC_METRICS_KERNELS_PSNR_METRIC_H

#include "ovt/abi/metric_v1.h"

namespace ovt::metrics {

class PluginRegistry;

// The function table of the psnr metric; static, never null.
[[nodiscard]] const ovt_MetricApi* psnr_metric_api() noexcept;

// Registers the in-tree kernel metrics (psnr, ssim; WI-3.308 adds ssim_x264, identity) in `registry`.
void register_builtin_kernels(PluginRegistry& registry);

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_KERNELS_PSNR_METRIC_H
