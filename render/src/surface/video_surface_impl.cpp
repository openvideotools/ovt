// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The one concrete VideoSurface (chapter 10 §10.1.3, §10.2.7). WI-1.400 is the skeleton: an OffscreenTexture target
// on libplacebo's Vulkan backend (MoltenVK on macOS, the Vulkan loader on Linux), cleared to
// ViewState.background_rgba by render(). WI-1.401 adds picture upload (slot A, §10.3.1) and draws it with a basic
// pl_render_image pass, with the §10.3.3 colour tables of WI-1.402. The full render parameters, target frames and
// dithering (WI-1.403), the compare modes of slot B (WI-1.410), interop (WI-1.404), native targets (WI-1.406 and
// later), overlays, measurements and export arrive with their own work items; until then those entry points are inert.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include "../overlay/overlay_layer.h"
#include "../overlay/overlay_set_impl.h"
#include "output_download.h"
#include "ovt/render/video_surface.h"
#include "pipeline/compare.h"
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
    overlay_gpu_ = std::make_unique<detail::OverlayLayerGpu>(vk_->gpu);  // WI-2.431
    compare_.init(log_, vk_->gpu);

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

  // Slot B and the mode (§10.2.4). SPEC-ASSUMPTION(#478): B is converted into A's grid here, so set_frame() comes
  // first when both change. RND-0010 / RND-0011 stay in last_error() while B is in that state.
  void set_compare(const Picture& b, const CompareMode& mode) override {
    if (vk_ == nullptr) {
      error_ = "RND-0001 picture rejected: surface is not initialised";
      return;
    }
    const bool restart_flicker =
        mode.kind == CompareMode::Kind::Flicker &&
        (mode_.kind != CompareMode::Kind::Flicker || mode.flicker_period_ms != mode_.flicker_period_ms);
    mode_ = mode;
    psnr_pending_ = false;
    psnr_ = {-1.0F, -1.0F, -1.0F};
    if (mode.kind == CompareMode::Kind::Single) {
      detail::destroy_slot(vk_->gpu, slot_b_);
      b_mismatch_ = false;
      b_converted_ = false;
      return;
    }
    if (restart_flicker) {
      flicker_start_ms_ = now_ms();
    }
    detail::PreparedB prep;
    std::string err = detail::prepare_b(b, detail::slot_format(slot_a_), mode.allow_scale_b, prep);
    if (err.empty()) {
      if (prep.converted || b_converted_) {
        slot_b_.valid = false;  // the same generation may now need another conversion
      }
      std::vector<std::string> notices;
      err = detail::upload_picture(vk_->gpu, prep.pic, slot_b_, notice_log_, notices);
      log_lines(notices);
    }
    if (!err.empty()) {
      slot_b_.valid = false;
      error_ = std::move(err);
      log_lines({error_});
      return;
    }
    b_mismatch_ = prep.size_mismatch;
    b_converted_ = prep.converted;
    error_ = prep.message;
    if (!error_.empty()) {
      log_lines({error_});
    }
  }
  // The CPU arrays of the set are uploaded here (§10.2.5: "GPU upload happens inside set_overlays()").
  void set_overlays(std::unique_ptr<OverlaySet> overlays) override {
    overlays_ = std::move(overlays);
    if (overlay_gpu_ == nullptr) {
      return;
    }
    std::string err;
    if (!overlay_gpu_->upload(overlays_.get(), err)) {
      error_ = "RND-0002 renderer init failed: overlays: " + err;
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
    }
  }

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
      if (mode_.kind == CompareMode::Kind::Single || !slot_b_.valid) {
        const detail::IRect full{0, 0, target_.width, target_.height};
        draw_slot(slot_a_, view, full, full, tex_, false, true);
      } else {
        render_compare(view);
      }
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

  // §10.7.9 measurements arrive with WI-4.452. SPEC-ASSUMPTION(#478): region_psnr (Difference) reduces the visible
  // region of the last render() here rather than in render(), so only a host that asks waits for the readback.
  Measurements measure() override {
    Measurements m;
    if (psnr_pending_ && slot_a_.valid && slot_b_.valid) {
      if (!compare_.psnr(slot_a_, slot_b_, psnr_region_, psnr_)) {
        error_ = "RND-0002 renderer init failed: compare: region PSNR pass failed";
      }
      psnr_pending_ = false;
    }
    for (size_t i = 0; i < 3; ++i) {
      m.region_psnr[i] = psnr_.at(i);  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }
    return m;
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

  void set_clock(std::function<int64_t()> clock) {
    clock_ = std::move(clock);
    flicker_start_ms_ = now_ms();
  }

 private:
  int64_t now_ms() const {
    if (clock_) {
      return clock_();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void log_lines(const std::vector<std::string>& lines) const {
    for (const std::string& line : lines) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), line.c_str());
    }
  }

  // Draws `slot` into `tex` with the §10.2.3 mapping of the viewport rectangle `view_rect`, clipped to `clip` (the
  // scissor of the side-by-side and split views). `linear` renders to an FP16 intermediate in linear light (the
  // target's primaries and peak, 1.0 = 203 nits) for the overlay blend; otherwise to the target's own encoding with
  // the full parameters of §10.3.4/§10.3.5.
  void draw_slot(const detail::FrameSlot& slot, const ViewState& view, const detail::IRect& clip,
                 const detail::IRect& view_rect, pl_tex tex, bool linear, bool with_overlays = false) {
    struct pl_frame image = slot.frame;
    pl_rect2df src = detail::source_rect(slot, view.show_cropped);
    pl_rect2df dst = detail::map_into(src, view, view_rect);
    const pl_rect2df unclipped = dst;  // the picture rectangle the overlay layer is laid out on
    // A scissor narrower than the target clips both rectangles; the whole target is left to libplacebo's own clipping.
    const bool whole = clip.x0 == 0 && clip.y0 == 0 && clip.x1 == target_.width && clip.y1 == target_.height;
    if (!whole && !detail::clip_rects(src, dst, clip)) {
      return;
    }
    image.crop = src;

    struct pl_frame target = {};
    target.num_planes = 1;
    target.planes[0].texture = tex;
    target.planes[0].components = 4;
    for (int c = 0; c < 4; ++c) {
      target.planes[0].component_mapping[c] = c;
    }
    const DisplayTarget::OutputEncoding encoding = detail::target_encoding(target_);
    target.color = detail::target_color(target_, encoding);
    if (linear) {
      target.color.transfer = PL_COLOR_TRC_LINEAR;
    }
    target.crop = dst;
    params_.build(view, linear ? DisplayTarget::OutputEncoding::ScRgbLinear16F : encoding,
                  target_.kind == DisplayTarget::Kind::OffscreenTexture, image.color, target);
    if (linear) {
      target.repr.bits = pl_bit_encoding{};  // a float texture holds normalised values; no integer depth scaling
    }
    // WI-2.431: the overlay layer, composited by libplacebo after the picture (§10.5, §10.5.6 step 1). It describes
    // slot A in the full viewport: drawn only where A is (Single, Flicker's A phase; SPEC-ASSUMPTION(#478)).
    struct pl_overlay overlay = {};
    std::string overlay_err;
    if (with_overlays && overlay_gpu_ != nullptr &&
        overlay_gpu_->draw(view, target_.width, target_.height, target_.device_pixel_ratio, unclipped, overlay,
                           overlay_err)) {
      target.overlays = &overlay;
      target.num_overlays = 1;
    } else if (!overlay_err.empty()) {
      error_ = "RND-0002 renderer init failed: overlays: " + overlay_err;
    }
    if (!pl_render_image(renderer_, &image, &target, &params_.params())) {
      error_ = "RND-0002 renderer init failed: render: pl_render_image failed";
    }
  }

  // The device-pixel bounding box of `slot` in the viewport (whole pixels, clipped to the target).
  [[nodiscard]] detail::IRect picture_box(const detail::FrameSlot& slot, const ViewState& view) const {
    const detail::IRect full{0, 0, target_.width, target_.height};
    const pl_rect2df d = detail::map_into(detail::source_rect(slot, view.show_cropped), view, full);
    return detail::intersect(full, detail::IRect{static_cast<int>(std::floor(d.x0)), static_cast<int>(std::floor(d.y0)),
                                                 static_cast<int>(std::ceil(d.x1)), static_cast<int>(std::ceil(d.y1))});
  }

  // §10.6.4 compare views of slots A and B.
  void render_compare(const ViewState& view) {
    const bool show_b = mode_.kind == CompareMode::Kind::Flicker &&
                        detail::flicker_shows_b(now_ms() - flicker_start_ms_, mode_.flicker_period_ms);
    const detail::CompareLayout layout = detail::compare_layout(mode_, target_.width, target_.height, show_b);
    const detail::IRect full{0, 0, target_.width, target_.height};
    const int enc = detail::shader_encoding(detail::target_encoding(target_));
    bool ok = true;
    switch (mode_.kind) {
      case CompareMode::Kind::Difference: {
        detail::DiffParams p;
        p.gain = mode_.diff_gain;
        p.plane = mode_.diff_plane >= 0 ? static_cast<int>(static_cast<uint8_t>(mode_.diff_plane)) : -1;  // -1: max
        p.signed_view = mode_.diff_signed;
        p.false_color = mode_.diff_false_color;
        p.hatch = b_mismatch_;
        p.encoding = enc;
        const pl_rect2df src = detail::source_rect(slot_a_, view.show_cropped);
        ok = compare_.difference(tex_, picture_box(slot_a_, view), slot_a_, slot_b_, src, view, full, p);
        psnr_pending_ = !b_mismatch_;
        psnr_region_ = detail::visible_samples(src, view, full);
        break;
      }
      case CompareMode::Kind::Overlay: {
        pl_tex ia = nullptr;
        pl_tex ib = nullptr;
        ok = compare_.intermediates(target_.width, target_.height, ia, ib);
        if (ok) {
          draw_slot(slot_a_, view, full, full, ia, true);
          draw_slot(slot_b_, view, full, full, ib, true);
          const detail::IRect a = picture_box(slot_a_, view);
          const detail::IRect b = picture_box(slot_b_, view);
          const detail::IRect box{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1),
                                  std::max(a.y1, b.y1)};
          ok = compare_.blend(tex_, box, mode_.overlay_alpha, enc);
        }
        break;
      }
      case CompareMode::Kind::Single:
      case CompareMode::Kind::SideBySide:
      case CompareMode::Kind::Split:
      case CompareMode::Kind::Flicker:
        if (layout.draw_a) {
          draw_slot(slot_a_, view, layout.clip_a, layout.view_a, tex_, false, mode_.kind == CompareMode::Kind::Flicker);
        }
        if (layout.draw_b) {
          draw_slot(slot_b_, view, layout.clip_b, layout.view_b, tex_, false);
        }
        if (!layout.divider.empty()) {
          ok = compare_.fill_white(tex_, layout.divider, enc);
        }
        break;
    }
    if (!ok) {
      error_ = "RND-0002 renderer init failed: compare: pass failed";
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
    overlay_gpu_.reset();
    if (vk_ != nullptr) {
      detail::destroy_slot(vk_->gpu, slot_a_);
      detail::destroy_slot(vk_->gpu, slot_b_);
      pl_tex_destroy(vk_->gpu, &tex_);
    }
    compare_.destroy();
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
  detail::FrameSlot slot_b_;  // compare picture, converted into A's grid (WI-1.410)
  CompareMode mode_;
  bool b_mismatch_ = false;   // RND-0010: B keeps its own size
  bool b_converted_ = false;  // slot B holds a converted copy
  detail::ComparePasses compare_;
  std::function<int64_t()> clock_;  // ms; steady_clock unless a test sets one (the host drives redraws, §10.6.4)
  int64_t flicker_start_ms_ = 0;
  bool psnr_pending_ = false;
  detail::IRect psnr_region_;
  std::array<float, 3> psnr_ = {-1.0F, -1.0F, -1.0F};
  detail::RenderParamSet params_;     // rebuilt for every render (§10.3.4)
  detail::H273NoticeLog notice_log_;  // RND-0020/RND-0021 once per colour description (§10.3.3)
  DisplayTarget target_{};
  std::unique_ptr<OverlaySet> overlays_;
  std::unique_ptr<detail::OverlayLayerGpu> overlay_gpu_;  // GPU side of overlays_ (WI-2.431)
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

void set_compare_clock(VideoSurface& surface, std::function<int64_t()> clock_ms) {
  auto* impl = dynamic_cast<VideoSurfaceImpl*>(&surface);
  if (impl != nullptr) {
    impl->set_clock(std::move(clock_ms));
  }
}

bool slot_a_chroma_shift(const VideoSurface& surface, float& shift_x, float& shift_y) {
  const auto* impl = dynamic_cast<const VideoSurfaceImpl*>(&surface);
  return impl != nullptr && impl->chroma_shift(shift_x, shift_y);
}

}  // namespace detail
}  // namespace ovt::render
