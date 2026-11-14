// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The GPU context and output texture of an OffscreenTexture target, one per row of the §10.2.2 interop table
// (chapter 10, WI-1.404):
//
//   Interop::None                the surface's own Vulkan device; read back with pl_tex_download (tests, CLI)
//   Interop::MetalTexture        the surface's own Vulkan device on MoltenVK; the output VkImage is created exportable
//                                and its id<MTLTexture> is handed to the host (macOS)
//   Interop::VulkanSharedDevice  the host's VkDevice through pl_vulkan_import; the VkImage is held for the host in
//                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL between renders (Linux)
//   Interop::GlTexture           the GL context current on the render thread (shared with the host's); the texture
//                                name is handed to the host with a fence (Android)
//
// Internal to libovt-render; VideoSurfaceImpl owns one OffscreenGpu per init().

#ifndef OVT_RENDER_SRC_PLATFORM_OFFSCREEN_H
#define OVT_RENDER_SRC_PLATFORM_OFFSCREEN_H

#include <cstdint>
#include <string>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>

#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

// The pl_fmt name of an offscreen format (libplacebo's "rgba16f" is an emulated 32-bit host format, hence "rgba16hf").
const char* offscreen_format_name(DisplayTarget::OffscreenFormat f);

class OffscreenGpu {
 public:
  OffscreenGpu() = default;
  OffscreenGpu(const OffscreenGpu&) = delete;
  OffscreenGpu& operator=(const OffscreenGpu&) = delete;
  OffscreenGpu(OffscreenGpu&&) = delete;
  OffscreenGpu& operator=(OffscreenGpu&&) = delete;
  ~OffscreenGpu() { destroy(); }

  // Sets up the pl_gpu that serves `target.interop`: `own_vk` (the surface's device, which the caller keeps owning)
  // for None and MetalTexture, the host's VkDevice or current GL context otherwise. On failure returns false with the
  // RND-0002 stage and reason.
  bool create(pl_log log, const DisplayTarget& target, pl_vulkan own_vk, std::string& stage, std::string& reason);
  // Releases the output texture and what create() made. The caller destroys its other pl_gpu objects (renderer,
  // uploaded planes) before, and its own Vulkan device after.
  void destroy();

  [[nodiscard]] pl_gpu gpu() const { return gpu_; }
  [[nodiscard]] DisplayTarget::Interop interop() const { return interop_; }

  // (Re)creates the output texture for `target` (size, offscreen_format). The texture is the libplacebo side's until
  // the first end_frame().
  bool create_texture(const DisplayTarget& target, std::string& reason);
  [[nodiscard]] pl_tex texture() const { return tex_; }

  // Takes the texture back from the host before libplacebo draws into or reads it (Vulkan: release the hold; GL: drop
  // the previous fence). Returns true when the texture had been handed to the host, so a caller that only reads it
  // (the CPU download) can hand it back with end_frame().
  bool begin_frame();
  // Submits the frame and hands the texture to the host under the platform's sync rule (§10.2.2): pl_gpu_flush for
  // every interop; VulkanSharedDevice holds the image in SHADER_READ_ONLY_OPTIMAL; MetalTexture waits for the GPU
  // (Qt samples on another MTLCommandQueue); GlTexture inserts a fence (or glFinish with OVT_RENDER_GL_FINISH=1).
  void end_frame();

  // The host's view of the output texture (§10.2.2 ExternalTexture).
  [[nodiscard]] ExternalTexture describe(const DisplayTarget& target) const;

 private:
  bool check_metal(std::string& stage, std::string& reason);
  bool import_vulkan(const DisplayTarget& target, std::string& stage, std::string& reason);
  bool create_gl(std::string& stage, std::string& reason);
  bool create_metal_texture(pl_fmt fmt, const DisplayTarget& target, std::string& reason);
  void destroy_texture();

  DisplayTarget::Interop interop_ = DisplayTarget::Interop::None;
  pl_log log_ = nullptr;
  pl_vulkan vk_ = nullptr;  // the surface's device (not owned) or the imported host device (owned)
  bool imported_ = false;
  pl_gpu gpu_ = nullptr;
  pl_tex tex_ = nullptr;
  bool held_ = false;  // VulkanSharedDevice: the image is held for the host (pl_vulkan_hold_ex)
  VkSemaphore hold_sem_ = VK_NULL_HANDLE;
  uint64_t hold_value_ = 0;
  // MetalTexture: the exportable VkImage behind the wrapped pl_tex, and its id<MTLTexture>.
  VkImage mtl_image_ = VK_NULL_HANDLE;
  VkDeviceMemory mtl_memory_ = VK_NULL_HANDLE;
  void* mtl_texture_ = nullptr;
  // GlTexture
  void* gl_ = nullptr;       // pl_opengl (kept opaque so this header needs no libplacebo/opengl.h)
  void* gl_sync_ = nullptr;  // GLsync of the last end_frame(), handed to the host as ExternalTexture.handle2
  bool gl_finish_ = false;   // OVT_RENDER_GL_FINISH=1
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PLATFORM_OFFSCREEN_H
