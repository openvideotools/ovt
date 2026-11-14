// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The one concrete VideoSurface (chapter 10 §10.1.3, §10.2.7). WI-1.400 is the skeleton: an OffscreenTexture target
// on libplacebo's Vulkan backend (MoltenVK on macOS, the Vulkan loader on Linux), cleared to
// ViewState.background_rgba by render(). WI-1.401 adds picture upload (slot A, §10.3.1) and draws it with a basic
// pl_render_image pass, with the §10.3.3 colour tables of WI-1.402. The full render parameters, target frames and
// dithering (WI-1.403), interop (WI-1.404), native targets (WI-1.406 and later), compare modes, overlays, measurements
// (WI-4.452, render/src/measure/) and export arrive with their own work items; until then those entry points are inert.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include "../overlay/overlay_set_impl.h"
#include "measure/measure.h"
#include "output_download.h"
#include "ovt/render/video_surface.h"
#include "pipeline/frame_upload.h"
#include "pipeline/render_params.h"
#include "vk_no_timestamps.h"

namespace ovt::render {
namespace {

// libplacebo's messages go to stderr with the surface's log prefix.
// SPEC-ASSUMPTION(#209): libovt_core exports no log emit function, so RND codes reach the host through last_error()
// and libplacebo's own messages through stderr until a C entry point exists.
void placebo_log_cb(void* priv, enum pl_log_level level, const char* msg) {
  const auto* prefix = static_cast<const std::string*>(priv);
  const char* tag = level <= PL_LOG_ERR ? "error" : (level == PL_LOG_WARN ? "warn" : "info");
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  (void)std::fprintf(stderr, "[%s] libplacebo %s: %s\n", prefix->c_str(), tag, msg);
}

enum pl_log_level placebo_log_level() {
  // §10.11 WI-1.400 notes: PL_LOG_WARN normally, PL_LOG_DEBUG behind OVT_RENDER_DEBUG=1.
  const char* debug = std::getenv("OVT_RENDER_DEBUG");  // NOLINT(concurrency-mt-unsafe): read once per init()
  return (debug != nullptr && std::strcmp(debug, "1") == 0) ? PL_LOG_DEBUG : PL_LOG_WARN;
}

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

class VideoSurfaceImpl final : public VideoSurface {
 public:
  explicit VideoSurfaceImpl(const char* log_prefix) : prefix_(log_prefix != nullptr ? log_prefix : "render") {}
  VideoSurfaceImpl(const VideoSurfaceImpl&) = delete;
  VideoSurfaceImpl& operator=(const VideoSurfaceImpl&) = delete;
  VideoSurfaceImpl(VideoSurfaceImpl&&) = delete;
  VideoSurfaceImpl& operator=(VideoSurfaceImpl&&) = delete;
  ~VideoSurfaceImpl() override { destroy(); }

  bool init(const DisplayTarget& target) override {
    destroy();
    error_.clear();
    if (target.kind != DisplayTarget::Kind::OffscreenTexture) {
      return fail("target", "only DisplayTarget::Kind::OffscreenTexture is implemented");
    }
    if (target.interop != DisplayTarget::Interop::None) {
      return fail("target", "offscreen interop is not implemented (WI-1.404)");
    }
    if (target.width <= 0 || target.height <= 0) {
      return fail("offscreen target", "width and height must be positive");
    }

    struct pl_log_params log_params = {};
    log_params.log_cb = placebo_log_cb;
    log_params.log_priv = &prefix_;
    log_params.log_level = placebo_log_level();
    log_ = pl_log_create(PL_API_VER, &log_params);
    if (log_ == nullptr) {
      return fail("pl_log", "pl_log_create failed");
    }

    const struct pl_vk_inst_params inst_params = pl_vk_inst_default_params;
    inst_ = pl_vk_inst_create(log_, &inst_params);
    if (inst_ == nullptr) {
      return fail("pl_vk_inst", "no Vulkan instance (Vulkan loader or MoltenVK missing)");
    }

    struct pl_vulkan_params vk_params = pl_vulkan_default_params;
    vk_params.instance = inst_->instance;
    // libplacebo loads its Vulkan functions through this wrapper, which hides timestamp support so libplacebo never
    // writes timer queries (MoltenVK resolves them after libplacebo may have freed the query pool;
    // vk_no_timestamps.cpp).
    vk_params.get_proc_addr = detail::no_timestamps_get_proc_addr(inst_->get_proc_addr);
    // Software rasterizers (lavapipe on GPU-less CI hosts) are accepted; libplacebo still prefers a hardware device.
    vk_params.allow_software = true;
    vk_ = pl_vulkan_create(log_, &vk_params);
    if (vk_ == nullptr) {
      return fail("pl_gpu", "pl_vulkan_create found no usable Vulkan device");
    }

    renderer_ = pl_renderer_create(log_, vk_->gpu);
    if (renderer_ == nullptr) {
      return fail("pl_renderer", "pl_renderer_create failed");
    }

    // §10.7: measurement passes. A GPU without what they need still renders; measure() then never becomes valid.
    static_cast<void>(measurer_.init(log_, vk_->gpu));

    target_ = target;
    return create_texture(target.width, target.height);
  }

  bool resize(int width, int height) override {
    if (vk_ == nullptr) {
      return fail("resize", "surface is not initialised");
    }
    if (width <= 0 || height <= 0) {
      return fail("resize", "width and height must be positive");
    }
    if (tex_ != nullptr && width == target_.width && height == target_.height) {
      return true;
    }
    target_.width = width;
    target_.height = height;
    return create_texture(width, height);
  }

  void retarget(const DisplayTarget& target) override {
    // Only OffscreenTexture exists so far (WI-1.406 adds NATIVE targets): a new size or format re-creates the output
    // texture on the same pl_gpu.
    if (vk_ == nullptr || target.kind != DisplayTarget::Kind::OffscreenTexture ||
        target.interop != DisplayTarget::Interop::None) {
      fail("retarget", "only OffscreenTexture without interop is implemented");
      return;
    }
    target_ = target;
    create_texture(target.width, target.height);
  }

  // SPEC-ASSUMPTION(#209): set_frame() is void (§10.2.7); a rejected picture leaves slot A empty and last_error()
  // reports RND-0001.
  void set_frame(const Picture& a) override {
    if (vk_ == nullptr) {
      error_ = "RND-0001 picture rejected: surface is not initialised";
      return;
    }
    std::vector<std::string> notices;
    std::string err = detail::upload_picture(vk_->gpu, a, slot_a_, notice_log_, notices);
    for (const std::string& line : notices) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), line.c_str());
    }
    if (!err.empty()) {
      error_ = std::move(err);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
      return;
    }
    error_.clear();
  }
  void set_compare(const Picture& /*b*/, const CompareMode& /*mode*/) override {}  // WI-1.410
  void set_overlays(std::unique_ptr<OverlaySet> overlays) override { overlays_ = std::move(overlays); }

  void render(const ViewState& view) override {
    if (tex_ == nullptr) {
      return;
    }
    // background_rgba is 0xRRGGBBAA in the output encoding (straight alpha).
    union pl_clear_color color = {};
    const uint32_t bg = view.background_rgba;
    color.f[0] = static_cast<float>((bg >> 24U) & 0xFFU) / 255.0F;
    color.f[1] = static_cast<float>((bg >> 16U) & 0xFFU) / 255.0F;
    color.f[2] = static_cast<float>((bg >> 8U) & 0xFFU) / 255.0F;
    color.f[3] = static_cast<float>(bg & 0xFFU) / 255.0F;
    pl_tex_clear_ex(vk_->gpu, tex_, color);
    if (slot_a_.valid) {
      draw_slot_a(view);
    }
    // §10.7.9: the measurement passes follow the display render in the same submission, only while measure() is
    // being called.
    if (measurer_.due()) {
      measurer_.run(slot_a_);
    }
    pl_gpu_flush(vk_->gpu);
  }

  [[nodiscard]] ExternalTexture output_texture() const override {
    // Interop::None: the handle is the pl_tex itself (only libovt-render dereferences it); WI-1.404 returns the
    // platform handles.
    ExternalTexture t{};
    t.interop = DisplayTarget::Interop::None;
    t.handle = const_cast<void*>(static_cast<const void*>(tex_));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    t.handle2 = nullptr;
    t.width = tex_ != nullptr ? target_.width : 0;
    t.height = tex_ != nullptr ? target_.height : 0;
    t.format = target_.offscreen_format;
    t.layout = 0;
    return t;
  }

  [[nodiscard]] Pick pick(int /*px*/, int /*py*/) const override { return Pick{}; }  // §10.5.7
  Measurements measure() override {                                                  // §10.7.9: never blocks
    measurer_.request();
    return measurer_.poll();
  }

  bool export_png(const char* /*path*/, ExportKind /*kind*/, const ViewState* /*view_or_null*/) override {
    error_ = "export_png is not implemented yet (chapter 10 §10.8)";
    return false;
  }

  [[nodiscard]] DisplayTarget::OutputEncoding active_encoding() const override {
    return detail::target_encoding(target_);
  }
  [[nodiscard]] const char* last_error() const override { return error_.c_str(); }

  bool chroma_shift(float& sx, float& sy) const {
    if (!slot_a_.valid || slot_a_.frame.num_planes < 3) {
      return false;
    }
    sx = slot_a_.frame.planes[1].shift_x;
    sy = slot_a_.frame.planes[1].shift_y;
    return true;
  }

  bool download(detail::OutputPixels& out) const {
    out = detail::OutputPixels{};
    if (tex_ == nullptr) {
      return false;
    }
    const size_t texel = tex_->params.format->texel_size;
    const auto w = static_cast<size_t>(target_.width);
    const auto h = static_cast<size_t>(target_.height);
    std::vector<uint8_t> bytes(w * h * texel);
    struct pl_tex_transfer_params tp = {};
    tp.tex = tex_;
    tp.row_pitch = w * texel;
    tp.ptr = bytes.data();
    if (!pl_tex_download(vk_->gpu, &tp)) {
      return false;
    }
    pl_gpu_finish(vk_->gpu);
    out.width = target_.width;
    out.height = target_.height;
    out.texel_bytes = texel;
    out.bytes = std::move(bytes);
    return true;
  }

 private:
  // Draws slot A over the cleared target with a basic parameter set: nearest upscaling at zoom >= 1, the ViewState
  // chroma upscaler and downscaler, no dithering, no anti-ringing (§10.3.1 "Scaling filters"). WI-1.403 replaces this
  // with the full pl_render_params of §10.3.4/§10.3.5 and the target frames of every encoding.
  void draw_slot_a(const ViewState& view) {
    struct pl_frame image = slot_a_.frame;
    image.crop = detail::source_rect(slot_a_, view.show_cropped);

    struct pl_frame target = {};
    target.num_planes = 1;
    target.planes[0].texture = tex_;
    target.planes[0].components = 4;
    for (int c = 0; c < 4; ++c) {
      target.planes[0].component_mapping[c] = c;
    }
    const DisplayTarget::OutputEncoding encoding = detail::target_encoding(target_);
    target.color = detail::target_color(target_, encoding);
    target.crop = detail::target_rect(image.crop, view, target_.width, target_.height);
    params_.build(view, encoding, target_.kind == DisplayTarget::Kind::OffscreenTexture, image.color, target);
    if (!pl_render_image(renderer_, &image, &target, &params_.params())) {
      error_ = "RND-0002 renderer init failed: render: pl_render_image failed";
    }
  }

  bool create_texture(int width, int height) {
    pl_gpu gpu = vk_->gpu;
    const char* name = offscreen_format_name(target_.offscreen_format);
    pl_fmt fmt = pl_find_named_fmt(gpu, name);
    constexpr auto kNeeded = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE |
                                                           PL_FMT_CAP_BLITTABLE | PL_FMT_CAP_HOST_READABLE);
    if (fmt == nullptr || (fmt->caps & kNeeded) != kNeeded) {
      pl_tex_destroy(gpu, &tex_);
      return fail("offscreen target", std::string("texture format ") + name + " is not renderable/blittable/readable");
    }
    struct pl_tex_params params = {};
    params.w = width;
    params.h = height;
    params.format = fmt;
    params.sampleable = true;
    params.renderable = true;
    params.blit_dst = true;
    params.host_readable = true;
    if (!pl_tex_recreate(gpu, &tex_, &params)) {
      return fail("offscreen target", std::string("pl_tex_recreate failed for ") + name);
    }
    return true;
  }

  bool fail(const std::string& stage, const std::string& reason) {
    error_ = "RND-0002 renderer init failed: " + stage + ": " + reason;  // §10.10
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
    return false;
  }

  // Waits until the driver has finished with every submission, including its own completion callbacks, before
  // pl_vulkan_destroy() frees the VkDevice.
  //
  // pl_vulkan_destroy() only waits on libplacebo's timeline semaphores. On MoltenVK those are MTLEvents that the GPU
  // signals from inside the command buffer, before Metal runs the command buffer's completed handlers on its
  // completion thread. MoltenVK registers several such handlers per submission (MVKQueueCommandBufferSubmission::
  // commitActiveMTLCommandBuffer records performance stats through the MVKDevice and then calls finish(); command
  // encoders return temporary MTLBuffer allocations to device-owned pools), and they dereference the MVKDevice
  // without holding a reference to it. A surface destroyed right after a download could free the device under a
  // handler that was still running: the intermittent SIGSEGV in teardown (#304, and again after it).
  //
  // vkDeviceWaitIdle alone does not close that window on MoltenVK 1.2.x: MVKQueue::waitIdle commits an empty command
  // buffer and returns from -waitUntilCompleted, which wakes when the buffer's status becomes completed, not when the
  // completion thread has run the earlier buffers' handlers. A fence does: a fence-only vkQueueSubmit becomes an
  // MVKQueueCommandBufferSubmission whose fence is signalled by finish(), from inside that buffer's own completed
  // handler. Metal runs a queue's completed handlers one after another in commit order, so once that fence is
  // signalled every handler of every earlier submission on the queue has returned. What the signalling handler still
  // does afterwards only releases objects it retained itself (the submission's fence and queue), never the device.
  //
  // On other drivers (lavapipe, desktop GPUs) this is a plain full wait: an empty submission with a fence is
  // signalled once all work previously submitted to the queue has completed (Vulkan 1.0 vkQueueSubmit).
  //
  // Every queue is locked through libplacebo for the whole wait: vkDeviceWaitIdle and vkQueueSubmit need external
  // synchronisation of the queues, and nothing may submit between the fence and the destruction.
  void wait_device_idle() {
    if (vk_ == nullptr || vk_->device == VK_NULL_HANDLE || vk_->get_proc_addr == nullptr) {
      return;
    }
    pl_gpu_finish(vk_->gpu);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto get_device_proc =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk_->get_proc_addr(vk_->instance, "vkGetDeviceProcAddr"));
    if (get_device_proc == nullptr) {
      return;
    }
    VkDevice dev = vk_->device;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(get_device_proc(dev, "vkDeviceWaitIdle"));
    const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(get_device_proc(dev, "vkGetDeviceQueue"));
    const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_device_proc(dev, "vkQueueSubmit"));
    const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_device_proc(dev, "vkCreateFence"));
    const auto wait_fences = reinterpret_cast<PFN_vkWaitForFences>(get_device_proc(dev, "vkWaitForFences"));
    const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_device_proc(dev, "vkDestroyFence"));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (device_wait_idle == nullptr || get_queue == nullptr || queue_submit == nullptr || create_fence == nullptr ||
        wait_fences == nullptr || destroy_fence == nullptr) {
      return;
    }
    // The graphics, compute and transfer families may coincide; lock each (family, index) once.
    std::vector<std::pair<uint32_t, uint32_t>> queues;
    for (const pl_vulkan_queue& q : {vk_->queue_graphics, vk_->queue_compute, vk_->queue_transfer}) {
      for (uint32_t i = 0; i < q.count; ++i) {
        const std::pair<uint32_t, uint32_t> key{q.index, i};
        if (std::find(queues.begin(), queues.end(), key) == queues.end()) {
          queues.push_back(key);
        }
      }
    }
    for (const auto& [qf, qi] : queues) {
      vk_->lock_queue(vk_, qf, qi);
    }
    static_cast<void>(device_wait_idle(dev));
    std::vector<VkFence> fences;
    fences.reserve(queues.size());
    for (const auto& [qf, qi] : queues) {
      VkQueue queue = VK_NULL_HANDLE;
      get_queue(dev, qf, qi, &queue);
      VkFenceCreateInfo info = {};
      info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      VkFence fence = VK_NULL_HANDLE;
      if (queue == VK_NULL_HANDLE || create_fence(dev, &info, nullptr, &fence) != VK_SUCCESS) {
        continue;
      }
      // submitCount 0 with a fence: the fence alone goes into the queue, behind everything submitted before it.
      if (queue_submit(queue, 0, nullptr, fence) != VK_SUCCESS) {
        destroy_fence(dev, fence, nullptr);
        continue;
      }
      fences.push_back(fence);
    }
    if (!fences.empty()) {
      static_cast<void>(wait_fences(dev, static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE, UINT64_MAX));
    }
    for (VkFence fence : fences) {
      destroy_fence(dev, fence, nullptr);
    }
    for (const auto& [qf, qi] : queues) {
      vk_->unlock_queue(vk_, qf, qi);
    }
  }

  void destroy() {
    overlays_.reset();
    measurer_.destroy();
    if (vk_ != nullptr) {
      detail::destroy_slot(vk_->gpu, slot_a_);
      pl_tex_destroy(vk_->gpu, &tex_);
    }
    pl_renderer_destroy(&renderer_);
    wait_device_idle();
    pl_vulkan_destroy(&vk_);
    pl_vk_inst_destroy(&inst_);
    pl_log_destroy(&log_);
  }

  std::string prefix_;
  std::string error_;
  pl_log log_ = nullptr;
  pl_vk_inst inst_ = nullptr;
  pl_vulkan vk_ = nullptr;
  pl_tex tex_ = nullptr;
  pl_renderer renderer_ = nullptr;
  detail::FrameSlot slot_a_;
  detail::RenderParamSet params_;     // rebuilt for every render (§10.3.4)
  detail::H273NoticeLog notice_log_;  // RND-0020/RND-0021 once per colour description (§10.3.3)
  DisplayTarget target_{};
  std::unique_ptr<OverlaySet> overlays_;
  detail::Measurer measurer_;  // §10.7 (WI-4.452)
};

}  // namespace

std::unique_ptr<VideoSurface> VideoSurface::create(const char* log_prefix) {
  return std::make_unique<VideoSurfaceImpl>(log_prefix);
}

namespace detail {

bool download_output(const VideoSurface& surface, OutputPixels& out) {
  const auto* impl = dynamic_cast<const VideoSurfaceImpl*>(&surface);
  if (impl == nullptr) {
    out = OutputPixels{};
    return false;
  }
  return impl->download(out);
}

bool slot_a_chroma_shift(const VideoSurface& surface, float& shift_x, float& shift_y) {
  const auto* impl = dynamic_cast<const VideoSurfaceImpl*>(&surface);
  return impl != nullptr && impl->chroma_shift(shift_x, shift_y);
}

}  // namespace detail
}  // namespace ovt::render
