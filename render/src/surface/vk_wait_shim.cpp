// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Why this exists: MoltenVK (1.2.11, unchanged on its main branch) implements vkWaitSemaphores on an MTLEvent-backed
// timeline semaphore by registering a notification block with -[MTLSharedEvent notifyListener:atValue:block:]. The
// block retains the semaphore, and when the event reaches the value it runs on the listener's dispatch queue:
//
//     lock_guard<mutex> blockLock(_lock);
//     if (_sitters.count(sitter)) { sitter->signaled(); }
//     release();      // may drop the last reference and delete the semaphore, _lock included
//
// A wait that returns VK_TIMEOUT leaves its block registered. libplacebo polls its command semaphores with a zero
// timeout all the time (vk_poll_commands(vk, 0) after every submit and in pl_tex/pl_buf polls), so most semaphores
// carry stale blocks, and each one fires later, when the GPU reaches that value. When the last command finishes,
// libplacebo's blocking wait wakes, and pl_vulkan_destroy destroys the semaphores straight away (vk_cmd_destroy:
// wait UINT64_MAX, then vkDestroySemaphore). A stale block running at that same moment on another listener's queue
// can then be left holding the last reference: release() deletes the semaphore while the block still holds its
// mutex, and the lock_guard unlocks freed memory. That is the intermittent SIGSEGV in surface teardown, in any cycle
// of render_test_plane_fill's teardown loop, that waiting for the device to idle cannot prevent: the blocks are not
// queue work.
//
// A blocking wait is safe: its own block wakes the waiter while holding the semaphore's mutex, and the waiter's
// unregisterWait() takes that mutex before vkWaitSemaphores returns, so the block has finished by then. A zero-timeout
// wait needs no block at all: it only asks whether the counter has reached the value, which vkGetSemaphoreCounterValue
// answers exactly (MoltenVK reads MTLSharedEvent.signaledValue). So the shim answers zero-timeout waits that way, and
// MoltenVK never registers a block it can leave behind. Semantics are the same on every driver, so the shim is not
// MoltenVK-specific and lavapipe exercises it in the Linux tests.

#include "vk_wait_shim.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace ovt::render::detail {
namespace {

struct DeviceFns {
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkWaitSemaphores wait = nullptr;
  PFN_vkGetSemaphoreCounterValue counter = nullptr;
  PFN_vkDestroyDevice destroy = nullptr;
};

std::atomic<PFN_vkGetInstanceProcAddr> g_real_get_instance_proc{nullptr};
std::atomic<PFN_vkGetDeviceProcAddr> g_real_get_device_proc{nullptr};
std::mutex g_devices_mutex;
std::vector<DeviceFns> g_devices;  // one entry per live VkDevice that libplacebo loaded functions for

bool same(const char* a, const char* b) {
  return std::strcmp(a, b) == 0;
}

bool lookup(VkDevice device, DeviceFns& out) {
  const std::lock_guard<std::mutex> lock(g_devices_mutex);
  for (const DeviceFns& d : g_devices) {
    if (d.device == device) {
      out = d;
      return true;
    }
  }
  return false;
}

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
DeviceFns register_device(VkDevice device) {
  const std::lock_guard<std::mutex> lock(g_devices_mutex);
  for (const DeviceFns& d : g_devices) {
    if (d.device == device) {
      return d;
    }
  }
  DeviceFns d;
  d.device = device;
  const PFN_vkGetDeviceProcAddr gdpa = g_real_get_device_proc.load();
  if (gdpa != nullptr) {
    d.wait = reinterpret_cast<PFN_vkWaitSemaphores>(gdpa(device, "vkWaitSemaphores"));
    if (d.wait == nullptr) {
      d.wait = reinterpret_cast<PFN_vkWaitSemaphores>(gdpa(device, "vkWaitSemaphoresKHR"));
    }
    d.counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(gdpa(device, "vkGetSemaphoreCounterValue"));
    if (d.counter == nullptr) {
      d.counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(gdpa(device, "vkGetSemaphoreCounterValueKHR"));
    }
    d.destroy = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device, "vkDestroyDevice"));
  }
  g_devices.push_back(d);
  return d;
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

VKAPI_ATTR VkResult VKAPI_CALL shim_wait_semaphores(VkDevice device, const VkSemaphoreWaitInfo* info,
                                                    uint64_t timeout) {
  DeviceFns d;
  if (!lookup(device, d) || d.wait == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;  // unreachable: the shim is only handed out for registered devices
  }
  if (timeout != 0 || d.counter == nullptr || info == nullptr) {
    return d.wait(device, info, timeout);
  }
  const bool any = (info->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0;
  for (uint32_t i = 0; i < info->semaphoreCount; ++i) {
    uint64_t value = 0;
    const VkResult r = d.counter(device, info->pSemaphores[i], &value);  // NOLINT(*-pointer-arithmetic)
    if (r != VK_SUCCESS) {
      return r;
    }
    const bool reached = value >= info->pValues[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (any && reached) {
      return VK_SUCCESS;
    }
    if (!any && !reached) {
      return VK_TIMEOUT;
    }
  }
  return any ? VK_TIMEOUT : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL shim_destroy_device(VkDevice device, const VkAllocationCallbacks* allocator) {
  PFN_vkDestroyDevice destroy = nullptr;
  {
    const std::lock_guard<std::mutex> lock(g_devices_mutex);
    for (auto it = g_devices.begin(); it != g_devices.end(); ++it) {
      if (it->device == device) {
        destroy = it->destroy;
        g_devices.erase(it);
        break;
      }
    }
  }
  if (destroy != nullptr) {
    destroy(device, allocator);
  }
}

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_get_device_proc(VkDevice device, const char* name) {
  const PFN_vkGetDeviceProcAddr real = g_real_get_device_proc.load();
  if (real == nullptr || name == nullptr) {
    return nullptr;
  }
  if (same(name, "vkWaitSemaphores") || same(name, "vkWaitSemaphoresKHR")) {
    const DeviceFns d = register_device(device);
    return d.wait != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(&shim_wait_semaphores) : nullptr;
  }
  if (same(name, "vkDestroyDevice")) {
    const DeviceFns d = register_device(device);
    return d.destroy != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(&shim_destroy_device) : nullptr;
  }
  return real(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_get_instance_proc(VkInstance instance, const char* name) {
  const PFN_vkGetInstanceProcAddr real = g_real_get_instance_proc.load();
  if (real == nullptr) {
    return nullptr;
  }
  if (name != nullptr && same(name, "vkGetDeviceProcAddr")) {
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(real(instance, name));
    if (gdpa == nullptr) {
      return nullptr;
    }
    PFN_vkGetDeviceProcAddr expected = nullptr;
    g_real_get_device_proc.compare_exchange_strong(expected, gdpa);
    return reinterpret_cast<PFN_vkVoidFunction>(&shim_get_device_proc);
  }
  return real(instance, name);
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace

PFN_vkGetInstanceProcAddr wait_shim_get_proc_addr(PFN_vkGetInstanceProcAddr real) {
  if (real == nullptr) {
    return nullptr;
  }
  PFN_vkGetInstanceProcAddr expected = nullptr;
  g_real_get_instance_proc.compare_exchange_strong(expected, real);
  return &shim_get_instance_proc;
}

}  // namespace ovt::render::detail
