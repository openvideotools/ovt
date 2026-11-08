// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A vkGetInstanceProcAddr wrapper that libovt-render hands to libplacebo (pl_vulkan_params.get_proc_addr). It answers
// vkWaitSemaphores calls with a zero timeout (libplacebo's non-blocking polls) from vkGetSemaphoreCounterValue
// instead of the driver's vkWaitSemaphores, and passes everything else through unchanged. See vk_wait_shim.cpp for
// the MoltenVK bug this avoids. Internal to libovt-render.

#ifndef OVT_RENDER_SRC_SURFACE_VK_WAIT_SHIM_H
#define OVT_RENDER_SRC_SURFACE_VK_WAIT_SHIM_H

#include <vulkan/vulkan_core.h>

namespace ovt::render::detail {

// Returns the wrapper for `real`, the instance's own vkGetInstanceProcAddr (pl_vk_inst::get_proc_addr). Every
// pl_vk_inst of the process comes from the same loader (or MoltenVK) and so has the same `real`; the wrapper keeps
// the first one it is given.
PFN_vkGetInstanceProcAddr wait_shim_get_proc_addr(PFN_vkGetInstanceProcAddr real);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_SURFACE_VK_WAIT_SHIM_H
