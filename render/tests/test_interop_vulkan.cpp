// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.404 acceptance test 2 (chapter 10 §10.11): Interop::VulkanSharedDevice. The test plays the host (Qt RHI on
// Linux, §10.2.2): it creates its own VkInstance and VkDevice, hands them to the surface through VulkanDeviceInfo,
// and reads the exported VkImage with its own commands on the shared queue. Runs wherever a Vulkan 1.2 device is
// available (lavapipe on CI Linux, MoltenVK on macOS); skips elsewhere.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include "crash_backtrace.h"
#include "ovt/render/video_surface.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::DisplayTarget;
using ovt::render::ExternalTexture;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::VulkanDeviceInfo;
using ovt::render::detail::OutputPixels;
using ovt::render::test::OwnedPicture;
using ovt::render::test::rgb_ramp;

// vkGetInstanceProcAddr of the Vulkan implementation libovt-render uses: already loaded into the process (the loader
// on Linux, MoltenVK on macOS), or the loader opened by name.
PFN_vkGetInstanceProcAddr find_get_instance_proc_addr() {
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): dlsym returns void*
  void* sym = dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr");
  if (sym == nullptr) {
    for (const char* name : {"libvulkan.so.1", "libvulkan.1.dylib", "libMoltenVK.dylib"}) {
      void* lib = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
      if (lib != nullptr) {
        sym = dlsym(lib, "vkGetInstanceProcAddr");
        break;
      }
    }
  }
  return reinterpret_cast<PFN_vkGetInstanceProcAddr>(sym);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

// A minimal Vulkan host: instance, device with libplacebo's required features, queue (family, 0), command pool.
class TestHost {
 public:
  TestHost() = default;
  TestHost(const TestHost&) = delete;
  TestHost& operator=(const TestHost&) = delete;
  TestHost(TestHost&&) = delete;
  TestHost& operator=(TestHost&&) = delete;
  ~TestHost() {
    if (device_ != VK_NULL_HANDLE) {
      fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device_);
      if (pool_ != VK_NULL_HANDLE) {
        fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device_, pool_, nullptr);
      }
      fn<PFN_vkDestroyDevice>("vkDestroyDevice")(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
      fn<PFN_vkDestroyInstance>("vkDestroyInstance")(instance_, nullptr);
    }
  }

  // Returns "" on success, else why no suitable device exists (the test skips).
  std::string create() {
    gipa_ = find_get_instance_proc_addr();
    if (gipa_ == nullptr) {
      return "no Vulkan implementation loaded";
    }
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ovt-render-interop-test";
    app.apiVersion = VK_API_VERSION_1_2;  // PL_VK_MIN_VERSION
    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    const auto create_instance = fn<PFN_vkCreateInstance>("vkCreateInstance");
    if (create_instance == nullptr || create_instance(&ici, nullptr, &instance_) != VK_SUCCESS) {
      return "vkCreateInstance failed";
    }
    uint32_t count = 0;
    fn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> phys(count);
    fn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance_, &count, phys.data());
    for (VkPhysicalDevice p : phys) {
      if (pick(p)) {
        break;
      }
    }
    if (phys_ == VK_NULL_HANDLE) {
      return "no Vulkan 1.2 device with a graphics queue, timelineSemaphore and hostQueryReset";
    }

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features f12 = {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.timelineSemaphore = VK_TRUE;  // pl_vulkan_required_features
    f12.hostQueryReset = VK_TRUE;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    // MoltenVK is a portability implementation: VK_KHR_portability_subset must be enabled when offered.
    std::vector<const char*> exts;
    if (has_device_extension("VK_KHR_portability_subset")) {
      exts.push_back("VK_KHR_portability_subset");
    }
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    if (fn<PFN_vkCreateDevice>("vkCreateDevice")(phys_, &dci, nullptr, &device_) != VK_SUCCESS) {
      return "vkCreateDevice failed";
    }
    fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device_, family_, 0, &queue_);
    VkCommandPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    REQUIRE(fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device_, &pci, nullptr, &pool_) == VK_SUCCESS);
    return "";
  }

  [[nodiscard]] VulkanDeviceInfo info() const {
    VulkanDeviceInfo i{};
    i.instance = instance_;
    i.physical_device = phys_;
    i.device = device_;
    i.queue_family = family_;
    i.queue_index = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): VulkanDeviceInfo keeps the PFN as void*
    i.get_instance_proc_addr = reinterpret_cast<void*>(gipa_);
    return i;
  }

  // Copies `image` (in `layout`, w x h, 4 bytes per texel) into host memory with the host's own commands on the
  // shared queue, as Qt's barrier + copy would, and leaves it in `layout` again.
  std::vector<uint8_t> read_image(VkImage image, VkImageLayout layout, uint32_t w, uint32_t h, size_t texel) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * texel;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    REQUIRE(fn<PFN_vkCreateBuffer>("vkCreateBuffer")(device_, &bci, nullptr, &buffer) == VK_SUCCESS);
    VkMemoryRequirements reqs = {};
    fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device_, buffer, &reqs);
    VkPhysicalDeviceMemoryProperties props = {};
    fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(phys_, &props);
    uint32_t type = UINT32_MAX;
    constexpr VkMemoryPropertyFlags kHostFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if ((reqs.memoryTypeBits & (1U << i)) != 0 && (props.memoryTypes[i].propertyFlags & kHostFlags) == kHostFlags) {
        type = i;
      }
    }
    REQUIRE(type != UINT32_MAX);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    REQUIRE(fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device_, &mai, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device_, buffer, memory, 0) == VK_SUCCESS);

    VkCommandBufferAllocateInfo cai = {};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    REQUIRE(fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device_, &cai, &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cmd, &begin) == VK_SUCCESS);

    VkImageMemoryBarrier to_src = {};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // The surface's work precedes this submission on the same queue; the barrier makes its writes visible.
    to_src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = layout;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const auto barrier = fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &to_src);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                                                             1, &region);
    VkImageMemoryBarrier back = to_src;  // return the image in the layout the host got it in
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = layout;
    VkBufferMemoryBarrier to_host = {};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = buffer;
    to_host.size = VK_WHOLE_SIZE;
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
            nullptr, 1, &to_host, 1, &back);
    REQUIRE(fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmd) == VK_SUCCESS);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    REQUIRE(fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    REQUIRE(fn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue_) == VK_SUCCESS);

    std::vector<uint8_t> out(size);
    void* mapped = nullptr;
    REQUIRE(fn<PFN_vkMapMemory>("vkMapMemory")(device_, memory, 0, size, 0, &mapped) == VK_SUCCESS);
    std::memcpy(out.data(), mapped, out.size());
    fn<PFN_vkUnmapMemory>("vkUnmapMemory")(device_, memory);
    fn<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device_, pool_, 1, &cmd);
    fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device_, buffer, nullptr);
    fn<PFN_vkFreeMemory>("vkFreeMemory")(device_, memory, nullptr);
    return out;
  }

 private:
  template <typename Fn>
  Fn fn(const char* name) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    return reinterpret_cast<Fn>(gipa_(instance_, name));
  }

  bool has_device_extension(const char* name) const {
    uint32_t n = 0;
    const auto enumerate = fn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    enumerate(phys_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    enumerate(phys_, nullptr, &n, props.data());
    for (const VkExtensionProperties& p : props) {
      if (std::strcmp(static_cast<const char*>(p.extensionName), name) == 0) {
        return true;
      }
    }
    return false;
  }

  bool pick(VkPhysicalDevice p) {
    VkPhysicalDeviceProperties props = {};
    fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(p, &props);
    if (props.apiVersion < VK_API_VERSION_1_2) {
      return false;
    }
    VkPhysicalDeviceVulkan12Features f12 = {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    fn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(p, &f2);
    if (f12.timelineSemaphore != VK_TRUE || f12.hostQueryReset != VK_TRUE) {
      return false;
    }
    uint32_t n = 0;
    const auto qf_props = fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    qf_props(p, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    qf_props(p, &n, families.data());
    for (uint32_t i = 0; i < n; ++i) {
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
        phys_ = p;
        family_ = i;
        return true;
      }
    }
    return false;
  }

  PFN_vkGetInstanceProcAddr gipa_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
};

}  // namespace

TEST_CASE("render_test_interop: VulkanSharedDevice exports a VkImage the host can read",
          "[fast][render][vulkan][interop]") {
  ovt::render::test::install_crash_backtrace();
  // The surface is destroyed before the host's device (the host outlives the surfaces it lends its device to).
  TestHost host;
  const std::string why = host.create();
  if (!why.empty()) {
    SKIP("no host Vulkan device: " << why);
  }
  ovt::render::test::pin_vulkan_icds();
  const VulkanDeviceInfo info = host.info();

  auto surface = VideoSurface::create("render-interop");
  REQUIRE(surface != nullptr);
  constexpr int kW = 32;
  constexpr int kH = 16;
  DisplayTarget t;
  t.width = kW;
  t.height = kH;
  t.interop = DisplayTarget::Interop::VulkanSharedDevice;
  t.interop_device = const_cast<VulkanDeviceInfo*>(&info);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  REQUIRE(surface->init(t));
  CHECK(std::string(surface->last_error()).empty());

  // Before the first render the image exists but is not the host's yet.
  ExternalTexture before = surface->output_texture();
  CHECK(before.interop == DisplayTarget::Interop::VulkanSharedDevice);
  CHECK(before.handle != nullptr);
  CHECK(before.layout == 0U);

  const OwnedPicture ramp = rgb_ramp(kW, kH);
  surface->set_frame(ramp.pic);
  REQUIRE(std::string(surface->last_error()).empty());

  for (int frame = 0; frame < 3; ++frame) {  // the hold/release cycle repeats per render
    INFO("frame " << frame);
    ViewState view;
    view.background_rgba = 0xFF00FFFFU;  // any pixel left at the background would show as magenta
    view.pan_x = kW / 2.0;               // picture centred: it covers the whole target at zoom 1
    view.pan_y = kH / 2.0;
    surface->render(view);
    const ExternalTexture tex = surface->output_texture();
    REQUIRE(tex.interop == DisplayTarget::Interop::VulkanSharedDevice);
    REQUIRE(tex.handle != nullptr);
    CHECK(tex.handle2 == nullptr);
    CHECK(tex.width == kW);
    CHECK(tex.height == kH);
    CHECK(tex.format == DisplayTarget::OffscreenFormat::Rgba8Srgb);
    CHECK(tex.layout == static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

    // The host reads the VkImage with its own commands on the shared queue ...
    const std::vector<uint8_t> host_bytes =
        host.read_image(static_cast<VkImage>(tex.handle), static_cast<VkImageLayout>(tex.layout), kW, kH, 4);
    // ... and gets what libplacebo's own download gives.
    OutputPixels px;
    REQUIRE(ovt::render::detail::download_output(*surface, px));
    REQUIRE(px.texel_bytes == 4);
    CHECK(host_bytes == px.bytes);
    size_t magenta = 0;
    for (size_t i = 0; i + 3 < host_bytes.size(); i += 4) {
      if (host_bytes[i] == 0xFF && host_bytes[i + 1] == 0x00 && host_bytes[i + 2] == 0xFF) {
        ++magenta;
      }
    }
    CHECK(magenta == 0);
    // The download handed the image back to the host in the same layout.
    CHECK(surface->output_texture().layout == static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
  }

  // A resize makes a new image; it is the host's again after the next render.
  REQUIRE(surface->resize(kW * 2, kH));
  surface->render(ViewState{});
  const ExternalTexture resized = surface->output_texture();
  CHECK(resized.width == kW * 2);
  CHECK(resized.layout == static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
  const std::vector<uint8_t> resized_bytes =
      host.read_image(static_cast<VkImage>(resized.handle), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kW * 2, kH, 4);
  OutputPixels px;
  REQUIRE(ovt::render::detail::download_output(*surface, px));
  CHECK(resized_bytes == px.bytes);

  surface.reset();  // the host's device survives the surface
}

TEST_CASE("render_test_interop: VulkanSharedDevice rejects incomplete device info", "[fast][render][interop]") {
  auto surface = VideoSurface::create("render-interop");
  DisplayTarget t;
  t.width = 16;
  t.height = 16;
  t.interop = DisplayTarget::Interop::VulkanSharedDevice;
  CHECK_FALSE(surface->init(t));  // interop_device == nullptr
  CHECK(std::string(surface->last_error()).rfind("RND-0002 renderer init failed: interop: ", 0) == 0);

  VulkanDeviceInfo info{};
  info.queue_index = 1;
  t.interop_device = &info;
  CHECK_FALSE(surface->init(t));  // null handles
  CHECK(std::string(surface->last_error()).rfind("RND-0002 renderer init failed: interop: ", 0) == 0);
  CHECK(surface->output_texture().handle == nullptr);
}
