// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Offscreen interop export (chapter 10 §10.2.2 interop table, WI-1.404). See offscreen.h for the four rows.

#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT 1  // VK_EXT_metal_objects (vulkan_metal.h); MTLTexture_id is void* outside ObjC
#endif

#include "offscreen.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/vulkan.h>

#include "surface/vk_no_timestamps.h"  // the host's device is loaded through it too

// GlTexture needs libplacebo's OpenGL backend (built on Android only, chapter 13 §13.3) and GLES 3.0 for the fence.
#if defined(OVT_RENDER_HAVE_GLES) && defined(PL_HAVE_OPENGL) && PL_HAVE_OPENGL
#define OVT_RENDER_GL_INTEROP 1
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <libplacebo/opengl.h>
#else
#define OVT_RENDER_GL_INTEROP 0
#endif

namespace ovt::render::detail {
namespace {

// Loads a device-level Vulkan function of `vk` (nullptr when unavailable).
template <typename Fn>
Fn device_fn(pl_vulkan vk, const char* name) {
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
  const auto get_device_proc =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk->get_proc_addr(vk->instance, "vkGetDeviceProcAddr"));
  return get_device_proc != nullptr ? reinterpret_cast<Fn>(get_device_proc(vk->device, name)) : nullptr;
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

template <typename Fn>
Fn instance_fn(pl_vulkan vk, const char* name) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
  return reinterpret_cast<Fn>(vk->get_proc_addr(vk->instance, name));
}

#if OVT_RENDER_GL_INTEROP
bool env_is_1(const char* name) {
  const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read once per init()
  return v != nullptr && std::strcmp(v, "1") == 0;
}
#endif

// The layout a held image has while the host samples it (§10.2.2 Linux row: Qt wraps {VkImage, layout}).
constexpr VkImageLayout kHostLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

}  // namespace

const char* offscreen_format_name(DisplayTarget::OffscreenFormat f) {
  switch (f) {
    case DisplayTarget::OffscreenFormat::Rgba8Srgb:
      return "rgba8";  // UNORM storage; the sRGB encoding is applied by the pipeline (§10.3.2), not the format
    case DisplayTarget::OffscreenFormat::Rgba16F:
      return "rgba16hf";  // IEEE half float ("rgba16f" is libplacebo's emulated 32-bit-host variant)
    case DisplayTarget::OffscreenFormat::Rgb10A2:
      return "rgb10a2";
  }
  return "rgba8";
}

bool OffscreenGpu::create(pl_log log, const DisplayTarget& target, pl_vulkan own_vk, std::string& stage,
                          std::string& reason) {
  destroy();
  log_ = log;
  interop_ = target.interop;
  switch (target.interop) {
    case DisplayTarget::Interop::None:
    case DisplayTarget::Interop::MetalTexture:
      vk_ = own_vk;  // the surface's own device (video_surface_impl.cpp creates and destroys it)
      gpu_ = vk_ != nullptr ? vk_->gpu : nullptr;
      return check_metal(stage, reason);
    case DisplayTarget::Interop::VulkanSharedDevice:
      return import_vulkan(target, stage, reason);
    case DisplayTarget::Interop::GlTexture:
      return create_gl(stage, reason);
  }
  stage = "target";
  reason = "unknown DisplayTarget::Interop";
  return false;
}

bool OffscreenGpu::check_metal(std::string& stage, std::string& reason) {
  stage = "interop";
  if (gpu_ == nullptr) {
    reason = "no Vulkan device";
    return false;
  }
  if (interop_ != DisplayTarget::Interop::MetalTexture) {
    return true;
  }
#if defined(VK_EXT_metal_objects)
  // §10.11 WI-1.404 notes: check the export capability before use. libplacebo cannot export a pl_tex as
  // PL_HANDLE_MTL_TEX itself (its Vulkan backend reports MTL_TEX for import only), so the texture is a VkImage
  // created here with VkExportMetalObjectCreateInfoEXT; what has to be there is the extension's entry point, which
  // libplacebo enables with its optional extensions when MoltenVK offers it.
  // SPEC-ASSUMPTION(#477): own exportable VkImage instead of pl_tex_params.export_handle.
  if (device_fn<PFN_vkExportMetalObjectsEXT>(vk_, "vkExportMetalObjectsEXT") != nullptr) {
    return true;
  }
  reason = "Interop::MetalTexture: the Vulkan device has no VK_EXT_metal_objects (MoltenVK >= 1.2.0 required)";
#else
  reason = "Interop::MetalTexture needs MoltenVK's VK_EXT_metal_objects (macOS only)";
#endif
  return false;
}

bool OffscreenGpu::import_vulkan(const DisplayTarget& target, std::string& stage, std::string& reason) {
  stage = "interop";
  const auto* info = static_cast<const VulkanDeviceInfo*>(target.interop_device);
  if (info == nullptr || info->instance == nullptr || info->physical_device == nullptr || info->device == nullptr ||
      info->get_instance_proc_addr == nullptr) {
    reason =
        "Interop::VulkanSharedDevice needs interop_device -> VulkanDeviceInfo with instance, physical_device, "
        "device and get_instance_proc_addr";
    return false;
  }
  // SPEC-ASSUMPTION(#477): libplacebo addresses queues 0..count-1 of a family and may rotate between
  // them, so "both use the same VkQueue" (§10.2.2) holds only for queue index 0 with count 1.
  if (info->queue_index != 0) {
    reason = "Interop::VulkanSharedDevice supports queue_index 0 only (libplacebo uses queues 0..n-1 of a family)";
    return false;
  }
  struct pl_vulkan_import_params params = {};
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the void* fields of VulkanDeviceInfo (R-ARCH-4)
  params.instance = static_cast<VkInstance>(info->instance);
  params.get_proc_addr =
      no_timestamps_get_proc_addr(reinterpret_cast<PFN_vkGetInstanceProcAddr>(info->get_instance_proc_addr));
  params.phys_device = static_cast<VkPhysicalDevice>(info->physical_device);
  params.device = static_cast<VkDevice>(info->device);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  params.queue_graphics.index = info->queue_family;
  params.queue_graphics.count = 1;
  // SPEC-ASSUMPTION(#477): VulkanDeviceInfo carries neither the enabled device extensions nor features.
  // The host MUST create its device with libplacebo's required features (Vulkan 1.2 timelineSemaphore and
  // hostQueryReset, pl_vulkan_required_features); no optional extension is assumed.
  params.features = &pl_vulkan_required_features;
  vk_ = pl_vulkan_import(log_, &params);
  if (vk_ == nullptr) {
    reason =
        "pl_vulkan_import rejected the host's VkDevice (Vulkan >= 1.2 with timelineSemaphore and "
        "hostQueryReset required)";
    return false;
  }
  imported_ = true;
  gpu_ = vk_->gpu;
  // The semaphore pl_vulkan_hold_ex signals when the image is the host's. The host does not wait on it (same queue,
  // §10.2.2); it exists because a hold needs one.
  struct pl_vulkan_sem_params sem_params = {};
  sem_params.type = VK_SEMAPHORE_TYPE_TIMELINE;
  sem_params.initial_value = 0;
  hold_sem_ = pl_vulkan_sem_create(gpu_, &sem_params);
  if (hold_sem_ == VK_NULL_HANDLE) {
    reason = "pl_vulkan_sem_create failed";
    return false;
  }
  return true;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): uses the members where the GL row is built
bool OffscreenGpu::create_gl(std::string& stage, std::string& reason) {
#if OVT_RENDER_GL_INTEROP
  // §10.2.2 Android row: the shell makes a context shared with Qt's current on the render thread before init()
  // (WI-A.403); libplacebo loads GL through EGL from that context.
  if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
    stage = "interop";
    reason = "Interop::GlTexture needs the shared EGL context current on the render thread";
    return false;
  }
  struct pl_opengl_params params = pl_opengl_default_params;
  params.get_proc_addr = eglGetProcAddress;
  params.egl_display = eglGetCurrentDisplay();
  params.egl_context = eglGetCurrentContext();
  params.allow_software = true;
  pl_opengl gl = pl_opengl_create(log_, &params);
  if (gl == nullptr) {
    stage = "pl_gpu";
    reason = "pl_opengl_create failed (no GL context current on the render thread?)";
    return false;
  }
  gl_ = const_cast<void*>(static_cast<const void*>(gl));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  gpu_ = gl->gpu;
  gl_finish_ = env_is_1("OVT_RENDER_GL_FINISH");
  return true;
#else
  stage = "target";
  reason = "Interop::GlTexture needs libplacebo's OpenGL backend (Android builds)";
  return false;
#endif
}

bool OffscreenGpu::create_texture(const DisplayTarget& target, std::string& reason) {
  const char* name = offscreen_format_name(target.offscreen_format);
  pl_fmt fmt = pl_find_named_fmt(gpu_, name);
  // The CPU download (Interop::None: tests, CLI) needs a host-readable format; the interop rows hand the texture to
  // the host and read it back only when the format allows (GLES drivers may not offer host reads of rgba8).
  auto needed = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_BLITTABLE);
  if (interop_ == DisplayTarget::Interop::None) {
    needed = static_cast<enum pl_fmt_caps>(needed | PL_FMT_CAP_HOST_READABLE);
  }
  if (fmt == nullptr || (fmt->caps & needed) != needed) {
    destroy_texture();
    reason = std::string("texture format ") + name + " is not renderable/blittable/readable";
    return false;
  }
  if (interop_ == DisplayTarget::Interop::MetalTexture) {
    destroy_texture();
    return create_metal_texture(fmt, target, reason);
  }
  begin_frame();  // a held image goes back to libplacebo before pl_tex_recreate replaces or keeps it
  struct pl_tex_params params = {};
  params.w = target.width;
  params.h = target.height;
  params.format = fmt;
  params.sampleable = true;
  params.renderable = true;
  params.blit_dst = true;
  params.host_readable = (fmt->caps & PL_FMT_CAP_HOST_READABLE) != 0;
  if (!pl_tex_recreate(gpu_, &tex_, &params)) {
    reason = std::string("pl_tex_recreate failed for ") + name;
    return false;
  }
  return true;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): uses the members where VK_EXT_metal_objects exists
bool OffscreenGpu::create_metal_texture(pl_fmt fmt, const DisplayTarget& target, std::string& reason) {
#if defined(VK_EXT_metal_objects)
  VkFormat vk_format = VK_FORMAT_R8G8B8A8_UNORM;  // libplacebo "rgba8"
  if (target.offscreen_format == DisplayTarget::OffscreenFormat::Rgba16F) {
    vk_format = VK_FORMAT_R16G16B16A16_SFLOAT;  // "rgba16hf"
  } else if (target.offscreen_format == DisplayTarget::OffscreenFormat::Rgb10A2) {
    vk_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // "rgb10a2"
  }
  const auto create_image = device_fn<PFN_vkCreateImage>(vk_, "vkCreateImage");
  const auto get_reqs = device_fn<PFN_vkGetImageMemoryRequirements>(vk_, "vkGetImageMemoryRequirements");
  const auto allocate = device_fn<PFN_vkAllocateMemory>(vk_, "vkAllocateMemory");
  const auto bind = device_fn<PFN_vkBindImageMemory>(vk_, "vkBindImageMemory");
  const auto export_objects = device_fn<PFN_vkExportMetalObjectsEXT>(vk_, "vkExportMetalObjectsEXT");
  const auto mem_props =
      instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>(vk_, "vkGetPhysicalDeviceMemoryProperties");
  if (create_image == nullptr || get_reqs == nullptr || allocate == nullptr || bind == nullptr ||
      export_objects == nullptr || mem_props == nullptr) {
    reason = "Vulkan entry points for the exportable Metal texture are missing";
    return false;
  }

  // Every queue family libplacebo submits to must be able to use the image (pl_vulkan_wrap_params.image).
  std::vector<uint32_t> families;
  for (const pl_vulkan_queue& q : {vk_->queue_graphics, vk_->queue_compute, vk_->queue_transfer}) {
    if (q.count > 0 && std::find(families.begin(), families.end(), q.index) == families.end()) {
      families.push_back(q.index);
    }
  }
  VkExportMetalObjectCreateInfoEXT export_info = {};
  export_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT;
  export_info.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.pNext = &export_info;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = vk_format;
  info.extent = {static_cast<uint32_t>(target.width), static_cast<uint32_t>(target.height), 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // sampleable, renderable, blit_dst (clears) and host_readable (download via a transfer) as for Interop::None
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
  info.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
  info.pQueueFamilyIndices = families.data();
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (create_image(vk_->device, &info, nullptr, &mtl_image_) != VK_SUCCESS) {
    reason = "vkCreateImage failed for the exportable Metal texture";
    return false;
  }
  VkMemoryRequirements reqs = {};
  get_reqs(vk_->device, mtl_image_, &reqs);
  VkPhysicalDeviceMemoryProperties props = {};
  mem_props(vk_->phys_device, &props);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): i < memoryTypeCount <= VK_MAX_MEMORY_TYPES
    const bool device_local = (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    if ((reqs.memoryTypeBits & (1U << i)) != 0 && device_local) {
      type = i;
    }
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = reqs.size;
  alloc.memoryTypeIndex = type;
  if (type == UINT32_MAX || allocate(vk_->device, &alloc, nullptr, &mtl_memory_) != VK_SUCCESS ||
      bind(vk_->device, mtl_image_, mtl_memory_, 0) != VK_SUCCESS) {
    reason = "no device-local memory for the exportable Metal texture";
    destroy_texture();
    return false;
  }

  VkExportMetalTextureInfoEXT tex_info = {};
  tex_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
  tex_info.image = mtl_image_;
  tex_info.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;  // single-plane image
  VkExportMetalObjectsInfoEXT objects = {};
  objects.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
  objects.pNext = &tex_info;
  export_objects(vk_->device, &objects);
  mtl_texture_ = tex_info.mtlTexture;  // MTLTexture_id is void* outside Objective-C
  if (mtl_texture_ == nullptr) {
    reason = "vkExportMetalObjectsEXT returned no MTLTexture";
    destroy_texture();
    return false;
  }

  struct pl_vulkan_wrap_params wrap = {};
  wrap.image = mtl_image_;
  wrap.width = target.width;
  wrap.height = target.height;
  wrap.format = vk_format;
  wrap.usage = info.usage;
  tex_ = pl_vulkan_wrap(gpu_, &wrap);
  if (tex_ == nullptr || tex_->params.format != fmt) {
    reason = "pl_vulkan_wrap failed for the exportable Metal texture";
    destroy_texture();
    return false;
  }
  // A wrapped image starts out held by the user; libplacebo owns it from here on. Metal has no image layouts, so the
  // host never hands it back (end_frame() waits for the GPU instead).
  struct pl_vulkan_release_params release = {};
  release.tex = tex_;
  release.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  release.qf = VK_QUEUE_FAMILY_IGNORED;
  pl_vulkan_release_ex(gpu_, &release);
  return true;
#else
  static_cast<void>(fmt);
  static_cast<void>(target);
  reason = "Interop::MetalTexture needs VK_EXT_metal_objects";
  return false;
#endif
}

bool OffscreenGpu::begin_frame() {
  if (tex_ == nullptr) {
    return false;
  }
  if (interop_ == DisplayTarget::Interop::VulkanSharedDevice && held_) {
    // The host has finished sampling: its commands precede ours on the same queue (§10.2.2), and it returned the
    // image in the layout it got it in.
    struct pl_vulkan_release_params release = {};
    release.tex = tex_;
    release.layout = kHostLayout;
    release.qf = VK_QUEUE_FAMILY_IGNORED;
    pl_vulkan_release_ex(gpu_, &release);
    held_ = false;
    return true;
  }
#if OVT_RENDER_GL_INTEROP
  if (interop_ == DisplayTarget::Interop::GlTexture && gl_sync_ != nullptr) {
    // The host waited on the fence (glWaitSync) before sampling the previous frame; it is not needed any more.
    glDeleteSync(static_cast<GLsync>(gl_sync_));
    gl_sync_ = nullptr;
    return true;
  }
#endif
  return false;
}

void OffscreenGpu::end_frame() {
  if (tex_ == nullptr) {
    return;
  }
  switch (interop_) {
    case DisplayTarget::Interop::None:
      pl_gpu_flush(gpu_);
      break;
    case DisplayTarget::Interop::MetalTexture:
      // Qt samples the MTLTexture on its own MTLCommandQueue; nothing orders that after MoltenVK's queue, so the frame
      // is complete before render() returns.
      pl_gpu_flush(gpu_);
      pl_gpu_finish(gpu_);
      break;
    case DisplayTarget::Interop::VulkanSharedDevice: {
      // Transition to the host's layout and mark the image held; the hold submits (pl_gpu_flush).
      struct pl_vulkan_hold_params hold = {};
      hold.tex = tex_;
      hold.layout = kHostLayout;
      hold.qf = VK_QUEUE_FAMILY_IGNORED;
      hold.semaphore.sem = hold_sem_;
      hold.semaphore.value = ++hold_value_;
      held_ = pl_vulkan_hold_ex(gpu_, &hold);
      pl_gpu_flush(gpu_);
      break;
    }
    case DisplayTarget::Interop::GlTexture:
      pl_gpu_flush(gpu_);
#if OVT_RENDER_GL_INTEROP
      if (gl_finish_) {
        glFinish();  // last-resort fallback (§10.2.2 Android row)
      } else {
        // GLES 3.0 fence on the surface's context; the host glWaitSync()s it on its own context of the share group.
        gl_sync_ = static_cast<void*>(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
        glFlush();  // the fence must reach the GPU before another context waits on it
      }
#endif
      break;
  }
}

ExternalTexture OffscreenGpu::describe(const DisplayTarget& target) const {
  ExternalTexture t{};
  t.interop = interop_;
  t.handle = nullptr;
  t.handle2 = nullptr;
  t.width = tex_ != nullptr ? target.width : 0;
  t.height = tex_ != nullptr ? target.height : 0;
  t.format = target.offscreen_format;
  t.layout = 0;
  if (tex_ == nullptr) {
    return t;
  }
  switch (interop_) {
    case DisplayTarget::Interop::None:
      // The handle is the pl_tex itself (only libovt-render dereferences it).
      t.handle = const_cast<void*>(static_cast<const void*>(tex_));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
      break;
    case DisplayTarget::Interop::MetalTexture:
      t.handle = mtl_texture_;  // id<MTLTexture>, owned by the surface (MoltenVK keeps it alive with the VkImage)
      break;
    case DisplayTarget::Interop::VulkanSharedDevice:
      t.handle = static_cast<void*>(pl_vulkan_unwrap(gpu_, tex_, nullptr, nullptr));
      // The layout the host finds the image in after render(); UNDEFINED (0) before the first render.
      t.layout = held_ ? static_cast<uint32_t>(kHostLayout) : 0U;
      break;
    case DisplayTarget::Interop::GlTexture:
#if OVT_RENDER_GL_INTEROP
      // GLuint texture name, cast; SPEC-ASSUMPTION(#477): handle2 carries the GLsync of the last render
      // (nullptr with OVT_RENDER_GL_FINISH=1), which the host glWaitSync()s before sampling.
      t.handle =
          reinterpret_cast<void*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
              static_cast<uintptr_t>(pl_opengl_unwrap(gpu_, tex_, nullptr, nullptr, nullptr)));
      t.handle2 = gl_sync_;
#endif
      break;
  }
  return t;
}

void OffscreenGpu::destroy_texture() {
  if (gpu_ == nullptr) {
    return;
  }
  begin_frame();
  if (interop_ == DisplayTarget::Interop::MetalTexture) {
    // The wrapper does not own the VkImage; the GPU must be done with it before the image and memory go.
    pl_gpu_finish(gpu_);
  }
  pl_tex_destroy(gpu_, &tex_);
#if defined(VK_EXT_metal_objects)
  if (mtl_image_ != VK_NULL_HANDLE || mtl_memory_ != VK_NULL_HANDLE) {
    const auto destroy_image = device_fn<PFN_vkDestroyImage>(vk_, "vkDestroyImage");
    const auto free_memory = device_fn<PFN_vkFreeMemory>(vk_, "vkFreeMemory");
    if (destroy_image != nullptr && mtl_image_ != VK_NULL_HANDLE) {
      destroy_image(vk_->device, mtl_image_, nullptr);
    }
    if (free_memory != nullptr && mtl_memory_ != VK_NULL_HANDLE) {
      free_memory(vk_->device, mtl_memory_, nullptr);
    }
  }
#endif
  mtl_image_ = VK_NULL_HANDLE;
  mtl_memory_ = VK_NULL_HANDLE;
  mtl_texture_ = nullptr;
}

void OffscreenGpu::destroy() {
  destroy_texture();
  if (vk_ != nullptr && hold_sem_ != VK_NULL_HANDLE) {
    pl_gpu_finish(gpu_);  // nothing may still signal the semaphore
    pl_vulkan_sem_destroy(gpu_, &hold_sem_);
  }
  hold_sem_ = VK_NULL_HANDLE;
  hold_value_ = 0;
  held_ = false;
  if (imported_) {
    // libplacebo's state on the host's device; the VkDevice stays the host's. The host may be using its queue, so a
    // pl_gpu_finish() (not the own-device teardown wait of video_surface_impl.cpp) is all that is needed.
    pl_gpu_finish(gpu_);
    pl_vulkan_destroy(&vk_);
  }
  vk_ = nullptr;
#if OVT_RENDER_GL_INTEROP
  if (gl_ != nullptr) {
    const auto* gl = static_cast<pl_opengl>(gl_);
    pl_gpu_finish(gl->gpu);
    pl_opengl_destroy(&gl);
  }
#endif
  gl_ = nullptr;
  gl_sync_ = nullptr;  // deleted with the context's share group
  gl_finish_ = false;
  gpu_ = nullptr;
  imported_ = false;
  interop_ = DisplayTarget::Interop::None;
}

}  // namespace ovt::render::detail
