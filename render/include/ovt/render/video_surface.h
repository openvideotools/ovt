// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// VideoSurface and its value types (chapter 10 §10.2.2–§10.2.4, §10.2.6, §10.2.7, verbatim; §0.6.4 is the abridged
// normative shape). No toolkit types (R-ARCH-4): native handles are void*.

#ifndef OVT_RENDER_VIDEO_SURFACE_H
#define OVT_RENDER_VIDEO_SURFACE_H

#include <cstdint>
#include <memory>

#include "ovt/render/export.h"  // ExportKind (§10.8)
#include "ovt/render/picture.h"
#include "ovt/render/render_api.h"
#include "ovt_model_generated.h"  // ovt::fb::FrameId

namespace ovt::render {

// NOLINTBEGIN(modernize-avoid-c-arrays,readability-isolate-declaration): declarations verbatim from §10.2

// ---- §10.2.2 DisplayTarget ------------------------------------------------------------------------------------
struct DisplayTarget {
  enum class Kind : uint8_t {
    OffscreenTexture,  // render into a texture the host composites (Qt QQuickRhiItem, CLI export, tests)
    MetalLayer,        // macOS: native = CAMetalLayer*
    AndroidWindow,     // Android: native = ANativeWindow* (from SurfaceView via ANativeWindow_fromSurface)
    WaylandSurface,    // Linux: native = wl_surface*, native2 = wl_display*
    VulkanSurface      // Any: native = VkSurfaceKHR, native2 = VkInstance (host created the surface, e.g.
                       // QVulkanInstance::surfaceForWindow)
  } kind = Kind::OffscreenTexture;
  void* native = nullptr;
  void* native2 = nullptr;
  int width = 0, height = 0;        // device pixels of the drawable
  float device_pixel_ratio = 1.0F;  // for label sizing; 2.0 on Retina
  // Display description (from display_probe(), §10.4.5). The surface treats these as the current output.
  bool hdr_capable = false;
  float sdr_white_nits = 203.0F;  // what "SDR white" means on this output in nits (BT.2408 reference = 203)
  float headroom = 1.0F;          // max output luminance / sdr_white (1.0 = SDR). macOS: NSScreen EDR value
  enum class OutputEncoding : uint8_t { Srgb8, ScRgbLinear16F, Pq10, Hlg10 } encoding = OutputEncoding::Srgb8;
  // OffscreenTexture only:
  enum class OffscreenFormat : uint8_t { Rgba8Srgb, Rgba16F, Rgb10A2 } offscreen_format = OffscreenFormat::Rgba8Srgb;
  enum class Interop : uint8_t { None, MetalTexture, VulkanSharedDevice, GlTexture } interop = Interop::None;
  void* interop_device = nullptr;  // VulkanSharedDevice: pointer to struct VulkanDeviceInfo (instance, phys, device,
                                   // queue family, queue index)
};

struct VulkanDeviceInfo {  // used when the host (Qt RHI, Vulkan backend) owns the device
  void* instance;
  void* physical_device;
  void* device;
  uint32_t queue_family;
  uint32_t queue_index;
  void* get_instance_proc_addr;  // PFN_vkGetInstanceProcAddr
};

struct ExternalTexture {  // how the host reads a texture the surface produced
  DisplayTarget::Interop interop;
  void* handle;   // id<MTLTexture> / VkImage / GLuint (cast)
  void* handle2;  // VkImageView or 0
  int width, height;
  DisplayTarget::OffscreenFormat format;
  uint32_t layout;  // VkImageLayout, 0 otherwise
};

// ---- §10.2.3 ViewState, ToneMap, GamutMap -------------------------------------------------------------------
enum class ToneMap : uint8_t {
  Auto,
  BT2390,
  BT2446A,
  Spline,
  Clip /* analysis: hard clip at clip_nits */,
  None /* pass-through */
};
enum class GamutMap : uint8_t {
  Perceptual,
  Relative,
  Saturation,
  Absolute,
  Desaturate,
  Darken,
  Clip,
  Highlight /* diagnostic */
};
enum class ChromaUpscale : uint8_t { Bilinear, Nearest };
enum class Downscale : uint8_t { Bilinear, Box, Lanczos };

struct ViewState {
  double zoom = 1.0;             // device pixels per luma sample; 1.0 = pixel-exact; nearest-neighbour when >= 1.0
  double pan_x = 0, pan_y = 0;   // image coordinate (luma samples, coded-picture space) shown at the viewport centre
  int plane_mode = 0;            // 0 YUV, 1 Y, 2 U, 3 V, 4 gray
  bool raw_code_values = false;  // plane modes 1-3: show code values without range expansion (engineering view)
  bool show_cropped = true;  // true: hide area outside crop (draw it dimmed at 30 %); false: show full coded picture
  ToneMap tone_map = ToneMap::Auto;
  GamutMap gamut_map = GamutMap::Perceptual;
  float clip_nits = 1000.0F;        // ToneMap::Clip threshold
  float target_peak_override = 0;   // 0 = use DisplayTarget.headroom*sdr_white; else force target peak (nits)
  bool false_color = false;         // §10.7.5
  bool gamut_clip_overlay = false;  // §10.7.8
  ChromaUpscale chroma_upscale = ChromaUpscale::Bilinear;
  Downscale downscale = Downscale::Bilinear;
  bool show_sample_values = true;  // text values when zoom >= 24
  bool dither = true;              // disabled automatically for float targets
  uint32_t background_rgba = 0x202020FF;
  uint16_t rotation = 0;  // 0, 90, 180, 270 degrees clockwise; default from TrackInfo.rotation (§10.6.1)
  enum class FieldMode : uint8_t {
    Weave,
    Frame,
    TopField,
    BottomField
  } field_mode = FieldMode::Weave;  // interlaced pictures (§10.6.1)
  bool show_field_tint = false;     // FieldMode::Frame: tint odd rows to make field parity visible
};

// ---- §10.2.4 CompareMode ------------------------------------------------------------------------------------
struct CompareMode {
  enum class Kind : uint8_t { Single, SideBySide, Split, Overlay, Difference, Flicker } kind = Kind::Single;
  enum class Axis : uint8_t { Vertical /* left|right */, Horizontal /* top/bottom */ } split_axis = Axis::Vertical;
  float split_position = 0.5F;       // Split: fraction of the viewport (0..1) where B begins
  float overlay_alpha = 0.5F;        // Overlay: B over A, alpha of B
  float diff_gain = 1.0F;            // Difference: |A-B| * gain (1, 2, 4, 8, 16, 32 in the GUI)
  bool diff_false_color = false;     // Difference: map via turbo LUT instead of gray
  int8_t diff_plane = -1;            // Difference: -1 = max over planes; 0 Y, 1 U, 2 V
  bool diff_signed = false;          // Difference: (A-B)*gain/2 + mid-grey instead of absolute
  uint16_t flicker_period_ms = 500;  // Flicker: A for period, then B for period
  bool allow_scale_b = false;  // if B's dimensions differ from A's: false = error state RND-0010; true = resample B to
                               // A (bilinear)
};

// ---- §10.2.5 (declared in overlay_set.h) --------------------------------------------------------------------
class OverlaySet;  // opaque; owns GPU buffers; created by a surface

// ---- §10.2.6 Pick and Measurements --------------------------------------------------------------------------
struct Pick {
  bool hit = false;                 // false when outside the picture
  int image_x = -1, image_y = -1;   // luma sample coordinates (coded picture)
  int cell_x = -1, cell_y = -1;     // image_x/4, image_y/4
  uint32_t block_id = 0xFFFFFFFFU;  // CellGrid.block_id at the cell (CPU lookup)
  int32_t mv_index = -1;  // index into FrameAnalysis.mvs if the cursor is on an arrow (GPU id buffer), else -1
  int32_t code[3] = {-1, -1,
                     -1};  // Y, U, V code values of picture A at the sample (chroma: co-sited sample per siting)
  int32_t code_b[3] = {-1, -1, -1};  // picture B when a compare mode is active
  float rgb_linear[3] = {0, 0, 0};   // linear light, content primaries, 1.0 = 203 nits (libplacebo convention)
  float nits_luminance = -1;         // Y of CIE XYZ in cd/m² (-1 when unavailable)
  float nits_maxrgb = -1;            // max(R,G,B) in cd/m²
  float pq_code = -1;                // PQ-encoded max(R,G,B) in [0,1]
  float cie_x = 0, cie_y = 0;
};

struct ScopeTexture {
  ExternalTexture tex;
  bool valid;
};

struct Measurements {
  bool valid = false;  // false until the first readback completes; results refer to measured_frame
  ovt::fb::FrameId measured_frame;
  float max_cll_nits = 0;            // max over pixels of max(R,G,B) in nits (CTA-861.3 definition)
  float max_cll_p9999_nits = 0;      // 99.99th percentile of the same quantity (from the 1024-bin PQ histogram)
  float fall_nits = 0;               // mean over pixels of max(R,G,B) in nits (frame-average light level)
  float min_nits = 0;                // min over pixels of max(R,G,B)
  uint32_t histogram_pq[1024] = {};  // bin i covers PQ code [i/1024, (i+1)/1024) of max(R,G,B); counts are over
                                     // *sampled* pixels (§10.7.3)
  uint32_t sampled_pixels = 0;       // number of pixels contributing (subsampling factor applied)
  uint32_t out_of_gamut_709 = 0, out_of_gamut_p3 = 0;  // sampled pixel counts outside BT.709 / P3 gamut (§10.7.8)
  float false_color_band_fraction[6] = {};             // fraction of pixels in each band of Table 10.7-1
  float region_psnr[3] = {-1, -1, -1};  // Difference mode only: PSNR (dB) of the visible region per plane,
                                        // code-value domain (§10.6.4)
  ScopeTexture waveform, vectorscope, cie_xy, histogram_image;
  double gpu_time_ms = 0;  // last render()+measure pass GPU time (0 if timers unavailable)
};

// ---- §10.2.7 VideoSurface -----------------------------------------------------------------------------------
// Threading: a VideoSurface is confined to the thread that called init() (the render thread).
class OVT_RENDER_API VideoSurface {
 public:
  static std::unique_ptr<VideoSurface> create(const char* log_prefix = "render");
  virtual ~VideoSurface() = default;

  // creates pl_gpu, swapchain or offscreen target; false on failure (log RND-0002..)
  virtual bool init(const DisplayTarget&) = 0;
  virtual bool resize(int width, int height) = 0;   // drawable size changed
  virtual void retarget(const DisplayTarget&) = 0;  // switch EMBEDDED <-> NATIVE; keeps uploaded frames
  virtual void set_frame(const Picture& a) = 0;     // slot A; uploads planes as R8/R16 textures
  virtual void set_compare(const Picture& b, const CompareMode&) = 0;  // slot B + mode; Kind::Single clears B
  virtual void set_overlays(std::unique_ptr<OverlaySet>) = 0;          // nullptr = no overlays
  virtual void render(const ViewState&) = 0;  // one frame; presents (native) or completes the texture (offscreen)
  [[nodiscard]] virtual ExternalTexture output_texture() const = 0;  // OffscreenTexture targets only
  [[nodiscard]] virtual Pick pick(int px, int py) const = 0;         // device pixels; uses last render()'s ViewState
  virtual Measurements measure() = 0;                                // non-blocking; see §10.7.9
  virtual bool export_png(const char* path, ExportKind, const ViewState* view_or_null) = 0;  // §10.8
  [[nodiscard]] virtual DisplayTarget::OutputEncoding active_encoding() const = 0;
  [[nodiscard]] virtual const char* last_error() const = 0;  // "" when none; stable code prefix "RND-nnnn"
};

// NOLINTEND(modernize-avoid-c-arrays,readability-isolate-declaration)

}  // namespace ovt::render

#endif  // OVT_RENDER_VIDEO_SURFACE_H
