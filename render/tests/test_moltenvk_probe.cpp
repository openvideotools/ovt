// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Probe of the prebuilt MoltenVK (WI-0.017, chapter 13 §13.3.1 and §13.5.6): libMoltenVK.dylib is a
// single-architecture Mach-O file of the architecture being built, and the directly linked Vulkan entry
// points work.

#include <catch2/catch_test_macros.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <fstream>

#ifndef OVT_MOLTENVK_DYLIB
#error "OVT_MOLTENVK_DYLIB must name the libMoltenVK.dylib the probe links"
#endif

namespace {

constexpr std::uint32_t kMachO64 = 0xfeedfacfU;  // MH_MAGIC_64, stored in host (little-endian) order
constexpr std::uint32_t kCpuArm64 = 0x0100000cU;
constexpr std::uint32_t kCpuX86_64 = 0x01000007U;

}  // namespace

TEST_CASE("libMoltenVK.dylib is a thin Mach-O of the built architecture", "[fast][third_party]") {
  std::ifstream file(OVT_MOLTENVK_DYLIB, std::ios::binary);
  REQUIRE(file.is_open());
  std::uint32_t header[2] = {0, 0};
  file.read(reinterpret_cast<char*>(header), sizeof header);
  REQUIRE(file.gcount() == static_cast<std::streamsize>(sizeof header));
  // A universal ("fat") file starts with 0xcafebabe (big-endian), which reads as 0xbebafeca here.
  REQUIRE(header[0] == kMachO64);
#if defined(__arm64__)
  CHECK(header[1] == kCpuArm64);
#elif defined(__x86_64__)
  CHECK(header[1] == kCpuX86_64);
#else
  FAIL("the MoltenVK probe runs on macOS only");
#endif
}

TEST_CASE("MoltenVK reports a Vulkan 1.2 instance", "[fast][third_party]") {
  std::uint32_t version = 0;
  REQUIRE(vkEnumerateInstanceVersion(&version) == VK_SUCCESS);
  CHECK(version >= VK_API_VERSION_1_2);

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "ovt-moltenvk-probe";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  REQUIRE(vkCreateInstance(&info, nullptr, &instance) == VK_SUCCESS);
  REQUIRE(instance != VK_NULL_HANDLE);
  // Whether a Metal device exists depends on the machine (headless VMs have none), so the count is only logged.
  std::uint32_t devices = 0;
  CHECK(vkEnumeratePhysicalDevices(instance, &devices, nullptr) == VK_SUCCESS);
  INFO("physical devices: " << devices);
  vkDestroyInstance(instance, nullptr);
}
