// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A vkGetInstanceProcAddr wrapper that libovt-render hands to libplacebo (pl_vulkan_params.get_proc_addr). It reports
// timestampValidBits = 0 for every queue family, so libplacebo never writes timestamp queries, and passes everything
// else through unchanged. See vk_no_timestamps.cpp for the MoltenVK use-after-free this avoids. Internal to
// libovt-render.

#ifndef OVT_RENDER_SRC_SURFACE_VK_NO_TIMESTAMPS_H
#define OVT_RENDER_SRC_SURFACE_VK_NO_TIMESTAMPS_H

#include <vulkan/vulkan_core.h>

namespace ovt::render::detail {

// Returns the wrapper around `real`, the instance's own vkGetInstanceProcAddr (pl_vk_inst::get_proc_addr). Every
// pl_vk_inst of the process comes from the same loader (or MoltenVK), so the wrapper keeps the first `real` it gets.
PFN_vkGetInstanceProcAddr no_timestamps_get_proc_addr(PFN_vkGetInstanceProcAddr real);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_SURFACE_VK_NO_TIMESTAMPS_H
