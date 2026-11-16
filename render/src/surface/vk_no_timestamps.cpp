// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Why this exists: libplacebo times every shader pass with a pl_timer, a VkQueryPool of timestamp queries (pl_dispatch
// creates one per pass and destroys it with the pl_renderer). It destroys a query pool once the GPU is idle, judged
// by its timeline semaphores (vk_gpu_idle_callback -> timer_destroy_cb -> vkDestroyQueryPool).
//
// MoltenVK (1.2.11, unchanged on its main branch) resolves the queries a command buffer wrote in a completed handler
// of the MTLCommandBuffer (MVKCommandEncoder::finishQueries):
//
//     [_mtlCmdBuffer addCompletedHandler: ^(id<MTLCommandBuffer>) {
//         for (auto& qryPair : *pAQs) { qryPair.first->finishQueries(...); }   // raw MVKQueryPool*, not retained
//         delete pAQs;
//     }];
//
// The timeline semaphore is an MTLEvent that the GPU signals inside the command buffer, before Metal runs completed
// handlers on its IOGPU notification thread. So libplacebo can see the work as finished and destroy the query pool
// (typically in pl_renderer_destroy at surface teardown) before that handler runs, and the handler then calls into
// and writes through a freed MVKQueryPool. The Mac Mini caught it directly (null dereference in the finishQueries
// block under -[_MTLCommandBuffer didCompleteWithStartTime:...]) and, far more often, as the heap damage it leaves:
// SIGSEGV at 0x60 or 0x298 in Metal's MTLDispatchListApply on the same thread (render_test_plane_fill teardown loop,
// #304, #330).
//
// libovt-render never reads libplacebo's pass timings (pl_dispatch_info), so the timers have no value here. libplacebo
// skips a timer on a queue family whose timestampValidBits is 0 (timer_begin in src/vulkan/gpu.c), and MoltenVK then
// never registers that handler. The wrapper reports 0 for every family, on every platform, so the Linux tests run the
// same path.

#include "vk_no_timestamps.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace ovt::render::detail {
namespace {

std::atomic<PFN_vkGetInstanceProcAddr> g_real_get_instance_proc{nullptr};
std::atomic<PFN_vkGetPhysicalDeviceQueueFamilyProperties> g_real_qf_props{nullptr};
std::atomic<PFN_vkGetPhysicalDeviceQueueFamilyProperties2> g_real_qf_props2{nullptr};

VKAPI_ATTR void VKAPI_CALL shim_qf_props(VkPhysicalDevice physical_device, uint32_t* count,
                                         VkQueueFamilyProperties* props) {
  const PFN_vkGetPhysicalDeviceQueueFamilyProperties real = g_real_qf_props.load();
  if (real == nullptr) {
    if (count != nullptr) {
      *count = 0;
    }
    return;
  }
  real(physical_device, count, props);
  if (props != nullptr && count != nullptr) {
    for (uint32_t i = 0; i < *count; ++i) {
      props[i].timestampValidBits = 0;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }
}

VKAPI_ATTR void VKAPI_CALL shim_qf_props2(VkPhysicalDevice physical_device, uint32_t* count,
                                          VkQueueFamilyProperties2* props) {
  const PFN_vkGetPhysicalDeviceQueueFamilyProperties2 real = g_real_qf_props2.load();
  if (real == nullptr) {
    if (count != nullptr) {
      *count = 0;
    }
    return;
  }
  real(physical_device, count, props);
  if (props != nullptr && count != nullptr) {
    for (uint32_t i = 0; i < *count; ++i) {
      props[i].queueFamilyProperties.timestampValidBits = 0;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }
}

std::atomic<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR> g_real_surface_formats{nullptr};
std::mutex g_filter_mutex;
std::map<VkSurfaceKHR, std::vector<VkSurfaceFormatKHR>> g_filters;  // guarded by g_filter_mutex

VKAPI_ATTR VkResult VKAPI_CALL shim_surface_formats(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                    uint32_t* count, VkSurfaceFormatKHR* formats) {
  const PFN_vkGetPhysicalDeviceSurfaceFormatsKHR real = g_real_surface_formats.load();
  std::vector<VkSurfaceFormatKHR> pairs;
  {
    const std::lock_guard<std::mutex> lock(g_filter_mutex);
    const auto it = g_filters.find(surface);
    if (it != g_filters.end()) {
      pairs = it->second;
    }
  }
  if (pairs.empty() || count == nullptr) {
    return real != nullptr ? real(physical_device, surface, count, formats) : VK_ERROR_INITIALIZATION_FAILED;
  }
  const auto n = static_cast<uint32_t>(pairs.size());
  if (formats == nullptr) {
    *count = n;
    return VK_SUCCESS;
  }
  const uint32_t copied = *count < n ? *count : n;
  std::memcpy(formats, pairs.data(), copied * sizeof(VkSurfaceFormatKHR));
  *count = copied;
  return copied < n ? VK_INCOMPLETE : VK_SUCCESS;
}

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_get_instance_proc(VkInstance instance, const char* name) {
  const PFN_vkGetInstanceProcAddr real = g_real_get_instance_proc.load();
  if (real == nullptr) {
    return nullptr;
  }
  const PFN_vkVoidFunction fn = real(instance, name);
  if (fn == nullptr || name == nullptr) {
    return fn;
  }
  if (std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) {
    g_real_qf_props.store(reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(fn));
    return reinterpret_cast<PFN_vkVoidFunction>(&shim_qf_props);
  }
  if (std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2") == 0 ||
      std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") == 0) {
    g_real_qf_props2.store(reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(fn));
    return reinterpret_cast<PFN_vkVoidFunction>(&shim_qf_props2);
  }
  if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) {
    g_real_surface_formats.store(reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(fn));
    return reinterpret_cast<PFN_vkVoidFunction>(&shim_surface_formats);
  }
  return fn;
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace

PFN_vkGetInstanceProcAddr no_timestamps_get_proc_addr(PFN_vkGetInstanceProcAddr real) {
  if (real == nullptr) {
    return nullptr;
  }
  PFN_vkGetInstanceProcAddr expected = nullptr;
  g_real_get_instance_proc.compare_exchange_strong(expected, real);
  return &shim_get_instance_proc;
}

void set_surface_format_filter(VkSurfaceKHR surface, std::vector<VkSurfaceFormatKHR> pairs) {
  const std::lock_guard<std::mutex> lock(g_filter_mutex);
  if (pairs.empty()) {
    g_filters.erase(surface);
  } else {
    g_filters[surface] = std::move(pairs);
  }
}

}  // namespace ovt::render::detail
