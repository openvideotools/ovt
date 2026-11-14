// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The one concrete VideoSurface (chapter 10 §10.1.3, §10.2.7). WI-1.400 is the skeleton: an OffscreenTexture target
// on libplacebo's Vulkan backend (MoltenVK on macOS, the Vulkan loader on Linux), cleared to
// ViewState.background_rgba by render(). WI-1.401 adds picture upload (slot A, §10.3.1) and draws it with a basic
// pl_render_image pass, with the §10.3.3 colour tables of WI-1.402. The full render parameters, target frames and
// dithering (WI-1.403), interop (WI-1.404), compare modes, overlays, measurements and export arrive with their own work
// items; until then those entry points are inert. WI-1.406 adds NATIVE targets: a libplacebo swapchain on the surface
// of a platform NativeOutput (src/platform/), retarget() EMBEDDED <-> NATIVE and the headroom poll of §10.4.1.

#include <algorithm>
#include <array>
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
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>

#include "../overlay/overlay_set_impl.h"
#include "output_download.h"
#include "ovt/render/video_surface.h"
#include "pipeline/frame_upload.h"
#include "pipeline/render_params.h"
#include "platform/native_target.h"
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
    const bool native = target.kind != DisplayTarget::Kind::OffscreenTexture;
    const detail::NativeProvider& provider = detail::native_provider();
    if (native && provider.create == nullptr) {
      return fail("target", "no NATIVE target of this kind on this platform");
    }
    if (!native && target.interop != DisplayTarget::Interop::None) {
      return fail("target", "offscreen interop is not implemented (WI-1.404)");
    }
    if (target.width <= 0 || target.height <= 0) {
      return fail(native ? "native target" : "offscreen target", "width and height must be positive");
    }
    std::unique_ptr<detail::NativeOutput> output;
    if (native) {
      std::string why;
      output = provider.create(target, why);
      if (output == nullptr) {
        return fail("target", why);
      }
    }

    struct pl_log_params log_params = {};
    log_params.log_cb = placebo_log_cb;
    log_params.log_priv = &prefix_;
    log_params.log_level = placebo_log_level();
    log_ = pl_log_create(PL_API_VER, &log_params);
    if (log_ == nullptr) {
      return fail("pl_log", "pl_log_create failed");
    }

    // The platform's surface extensions are optional on every surface, so an OffscreenTexture surface can retarget()
    // to NATIVE later on the same instance.
    struct pl_vk_inst_params inst_params = pl_vk_inst_default_params;
    inst_params.opt_extensions = provider.instance_extensions.data();
    inst_params.num_opt_extensions = static_cast<int>(provider.instance_extensions.size());
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
    // The device can present whenever the instance can (an OffscreenTexture surface may retarget() to NATIVE).
    static constexpr std::array<const char*, 1> kSwapchainExt = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vk_params.opt_extensions = kSwapchainExt.data();
    vk_params.num_opt_extensions = static_cast<int>(kSwapchainExt.size());
    if (output != nullptr) {
      // libplacebo picks a queue family that can present to the surface (pl_vulkan_params.surface).
      std::string why;
      VkSurfaceKHR surf = output->create_surface(inst_->instance, inst_->get_proc_addr, why);
      if (surf == VK_NULL_HANDLE) {
        return fail("native target", why);
      }
      // Owned from here on: destroy() releases them if a later step fails.
      native_ = std::move(output);
      surf_ = surf;
      vk_params.surface = surf;
    }
    vk_ = pl_vulkan_create(log_, &vk_params);
    if (vk_ == nullptr) {
      return fail("pl_gpu", "pl_vulkan_create found no usable Vulkan device");
    }

    renderer_ = pl_renderer_create(log_, vk_->gpu);
    if (renderer_ == nullptr) {
      return fail("pl_renderer", "pl_renderer_create failed");
    }

    target_ = target;
    if (native_ != nullptr) {
      return open_native();
    }
    active_ = detail::target_encoding(target_);
    return create_texture(target.width, target.height);
  }

  bool resize(int width, int height) override {
    if (vk_ == nullptr) {
      return fail("resize", "surface is not initialised");
    }
    if (width <= 0 || height <= 0) {
      return fail("resize", "width and height must be positive");
    }
    if ((tex_ != nullptr || sw_ != nullptr) && width == target_.width && height == target_.height) {
      return true;
    }
    target_.width = width;
    target_.height = height;
    if (sw_ != nullptr) {
      return resize_swapchain();
    }
    return create_texture(width, height);
  }

  // Switches between EMBEDDED (OffscreenTexture) and NATIVE targets, or updates the current one (size, encoding,
  // headroom), on the same pl_gpu: uploaded pictures and overlays are kept (§10.2.7).
  void retarget(const DisplayTarget& target) override {
    if (vk_ == nullptr) {
      fail("retarget", "surface is not initialised");
      return;
    }
    if (target.width <= 0 || target.height <= 0) {
      fail("retarget", "width and height must be positive");
      return;
    }
    if (target.kind == DisplayTarget::Kind::OffscreenTexture) {
      if (target.interop != DisplayTarget::Interop::None) {
        fail("retarget", "offscreen interop is not implemented (WI-1.404)");
        return;
      }
      close_native();
      target_ = target;
      active_ = detail::target_encoding(target_);
      create_texture(target.width, target.height);
      return;
    }
    if (sw_ != nullptr && target.kind == target_.kind && target.native == target_.native &&
        target.native2 == target_.native2) {
      // Same native target: a new encoding re-picks the format pair, a new size resizes, a new headroom only changes
      // the target peak of the next render (libplacebo re-derives the tone curve; no swapchain recreation).
      const bool encoding_changed = target.encoding != target_.encoding || target.hdr_capable != target_.hdr_capable;
      const bool size_changed = target.width != target_.width || target.height != target_.height;
      target_ = target;
      if (encoding_changed) {
        open_native();  // a new swapchain: libplacebo reads the surface formats only when it creates one
      } else if (size_changed) {
        resize_swapchain();
      }
      return;
    }
    const detail::NativeProvider& provider = detail::native_provider();
    if (provider.create == nullptr) {
      fail("retarget", "no NATIVE target of this kind on this platform");
      return;
    }
    std::string why;
    std::unique_ptr<detail::NativeOutput> output = provider.create(target, why);
    if (output == nullptr) {
      fail("retarget", why);
      return;
    }
    VkSurfaceKHR surf = output->create_surface(vk_->instance, vk_->get_proc_addr, why);
    if (surf == VK_NULL_HANDLE) {
      fail("retarget", why);
      return;
    }
    close_native();
    pl_tex_destroy(vk_->gpu, &tex_);
    native_ = std::move(output);
    surf_ = surf;
    target_ = target;
    open_native();
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
    if (sw_ != nullptr) {
      render_native(view);
      return;
    }
    if (tex_ == nullptr) {
      return;
    }
    pl_tex_clear_ex(vk_->gpu, tex_, background(view));
    if (slot_a_.valid) {
      struct pl_frame target = {};
      target.num_planes = 1;
      target.planes[0].texture = tex_;
      target.planes[0].components = 4;
      for (int c = 0; c < 4; ++c) {
        target.planes[0].component_mapping[c] = c;
      }
      target.color = detail::target_color(target_, active_);
      draw_slot_a(view, target, target_.kind == DisplayTarget::Kind::OffscreenTexture);
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
  Measurements measure() override { return Measurements{}; }                         // §10.7.9

  bool export_png(const char* /*path*/, ExportKind /*kind*/, const ViewState* /*view_or_null*/) override {
    error_ = "export_png is not implemented yet (chapter 10 §10.8)";
    return false;
  }

  [[nodiscard]] DisplayTarget::OutputEncoding active_encoding() const override { return active_; }
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
  // background_rgba is 0xRRGGBBAA in the output encoding (straight alpha).
  static union pl_clear_color background(const ViewState& view) {
    union pl_clear_color color = {};
    const uint32_t bg = view.background_rgba;
    color.f[0] = static_cast<float>((bg >> 24U) & 0xFFU) / 255.0F;
    color.f[1] = static_cast<float>((bg >> 16U) & 0xFFU) / 255.0F;
    color.f[2] = static_cast<float>((bg >> 8U) & 0xFFU) / 255.0F;
    color.f[3] = static_cast<float>(bg & 0xFFU) / 255.0F;
    return color;
  }

  // Draws slot A into `target` (whose planes and colour space the caller set) with the §10.3.4/§10.3.5 parameters.
  void draw_slot_a(const ViewState& view, struct pl_frame& target, bool offscreen) {
    struct pl_frame image = slot_a_.frame;
    image.crop = detail::source_rect(slot_a_, view.show_cropped);
    target.crop = detail::target_rect(image.crop, view, target_.width, target_.height);  // Vulkan images never flip
    params_.build(view, active_, offscreen, image.color, target);
    if (!pl_render_image(renderer_, &image, &target, &params_.params())) {
      error_ = "RND-0002 renderer init failed: render: pl_render_image failed";
    }
  }

  // One render of a NATIVE target: polls the headroom, acquires a swapchain image, draws and presents (§10.4.1).
  void render_native(const ViewState& view) {
    const float polled = native_->polled_headroom();
    if (active_ != DisplayTarget::OutputEncoding::Srgb8 && detail::headroom_changed(target_.headroom, polled)) {
      DisplayTarget updated = target_;
      updated.headroom = polled;
      retarget(updated);
    }
    struct pl_swapchain_frame frame = {};
    if (!pl_swapchain_start_frame(sw_, &frame)) {
      return;  // no image now (window hidden or minimised); not an error
    }
    struct pl_frame target = {};
    pl_frame_from_swapchain(&target, &frame);
    target.color = detail::native_target_color(target_, active_);
    if (frame.fbo->params.blit_dst) {
      pl_tex_clear_ex(vk_->gpu, frame.fbo, background(view));
    }
    if (slot_a_.valid) {
      draw_slot_a(view, target, false);
    }
    if (!pl_swapchain_submit_frame(sw_)) {
      error_ = "RND-0002 renderer init failed: render: pl_swapchain_submit_frame failed";
      return;
    }
    pl_swapchain_swap_buffers(sw_);
  }

  // (Re)creates the swapchain on surf_ for target_: picks the encoding from the surface's formats, lets libplacebo see
  // only that encoding's pairs, and configures the platform object before the swapchain's first present (§10.4.1).
  bool open_native() {
    if (sw_ != nullptr) {
      wait_device_idle();
      pl_swapchain_destroy(&sw_);
    }
    const detail::NativeChoice choice = detail::choose_native_encoding(target_, surface_formats());
    active_ = choice.encoding;
    if (choice.no_hdr_format) {
      error_ = "RND-0040 no HDR surface format; falling back to Srgb8";
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
    }
    detail::set_surface_format_filter(surf_, choice.pairs);
    native_->configure(active_, target_.width, target_.height);
    struct pl_vulkan_swapchain_params sp = {};
    sp.surface = surf_;
    sp.present_mode = VK_PRESENT_MODE_FIFO_KHR;  // §10.4.1
    sp.disable_10bit_sdr = true;                 // Srgb8 is an 8-bit target (§10.3.2)
    sw_ = pl_vulkan_create_swapchain(vk_, &sp);
    if (sw_ == nullptr) {
      close_native();
      return fail("swapchain", "pl_vulkan_create_swapchain failed");
    }
    pl_swapchain_colorspace_hint(sw_, &choice.hint);
    return resize_swapchain();
  }

  // Not an error when it fails: libplacebo cannot size a swapchain on a 0x0 extent (hidden window, layer not laid out)
  // and creates it at the first pl_swapchain_start_frame() with an extent; render() draws nothing until then.
  bool resize_swapchain() {
    native_->configure(active_, target_.width, target_.height);
    int w = target_.width;
    int h = target_.height;
    static_cast<void>(pl_swapchain_resize(sw_, &w, &h));
    return true;
  }

  [[nodiscard]] std::vector<VkSurfaceFormatKHR> surface_formats() const {
    std::vector<VkSurfaceFormatKHR> formats;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto get_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(  // unfiltered: the loader's
        inst_->get_proc_addr(inst_->instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    uint32_t n = 0;
    if (get_formats == nullptr || get_formats(vk_->phys_device, surf_, &n, nullptr) != VK_SUCCESS) {
      return formats;
    }
    formats.resize(n);
    if (get_formats(vk_->phys_device, surf_, &n, formats.data()) != VK_SUCCESS) {
      formats.clear();
    }
    formats.resize(n);
    return formats;
  }

  void destroy_vk_surface(VkSurfaceKHR surf) const {
    if (surf == VK_NULL_HANDLE || inst_ == nullptr) {
      return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto destroy_surface =
        reinterpret_cast<PFN_vkDestroySurfaceKHR>(inst_->get_proc_addr(inst_->instance, "vkDestroySurfaceKHR"));
    if (destroy_surface != nullptr) {
      destroy_surface(inst_->instance, surf, nullptr);
    }
  }

  // Destroys the swapchain, its surface and the platform object (the device stays).
  void close_native() {
    if (sw_ != nullptr) {
      wait_device_idle();
      pl_swapchain_destroy(&sw_);
    }
    detail::set_surface_format_filter(surf_, {});
    destroy_vk_surface(surf_);
    surf_ = VK_NULL_HANDLE;
    native_.reset();
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
    if (vk_ != nullptr) {
      detail::destroy_slot(vk_->gpu, slot_a_);
      pl_tex_destroy(vk_->gpu, &tex_);
    }
    pl_renderer_destroy(&renderer_);
    close_native();
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
  DisplayTarget::OutputEncoding active_ = DisplayTarget::OutputEncoding::Srgb8;
  std::unique_ptr<OverlaySet> overlays_;
  // NATIVE targets (WI-1.406): the platform object, its surface and libplacebo's swapchain on it.
  std::unique_ptr<detail::NativeOutput> native_;
  VkSurfaceKHR surf_ = VK_NULL_HANDLE;
  pl_swapchain sw_ = nullptr;
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
