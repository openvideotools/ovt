# 10 — Rendering, colour management and HDR

> **Scope.** This chapter specifies `libovt-render` (`/render/`, LGPL-2.1-or-later): the `VideoSurface` that draws decoded pictures with libplacebo (ADR-005), the overlay renderer (grids, motion-vector arrows, heatmaps, labels, picking), the per-platform HDR output paths (ADR-004, ADR-014), the per-plane and diagnostic views, and the GPU-side HDR measurements. It conforms to the backbone (§0.5 names, §0.6.4 interface, §0.7 budgets). The GUI that drives it is specified in Chapter 11; the CLI entry points (`ovt render`, `ovt hdr --measure`) in Chapter 12.
>
> **Reader.** An implementer who knows C++20 and basic GPU programming but not colour science. Every colour-science term is in the Glossary (Chapter 2) or in §11 below.

---

## 10.1 Goals and architecture

### 10.1.1 Requirements realised by this chapter

Requirement ids are defined only in chapter 04 (§4.5 BLK, §4.6 PLN, §4.7 HDR, §4.9 GUI, §4.15 NFR). Each row copies chapter 04's sentence, names where this chapter realises it, and gives the acceptance test this chapter adds on top of chapter 04's. Two requirements this chapter needed were added to chapter 04 as FR-HDR-101 and FR-PLN-101.

| Id | Chapter 04 requirement (verbatim) | Realised in | Chapter 10 acceptance test |
|---|---|---|---|
| **FR-HDR-003** | On an HDR-capable display, `VideoSurface` MUST output PQ content without tone mapping when the display headroom allows (content MaxCLL ≤ display peak) and MUST output HLG via the BT.2100 OOTF with system gamma `1.2 + 0.42 · log10(L_W / 1000)` for the display peak `L_W`, with `ToneMap::Auto` selecting BT.2390 when the display peak is below content peak. | §10.3.2, §10.3.4, §10.4, WI-1.403, WI-1.406, WI-4.450, WI-A.401 | Given `tests/corpora/patterns/pq_patches_1000.mp4` (id `syn_pat_pq_patches_1000`), When rendered in NATIVE mode on a display with ≥ 1000-nit headroom, Then a colorimeter reads the 203-nit patch within ±10 % of the system UI white and the 1000-nit patch at (4.9 ± 0.5) × the 203-nit patch (ratios: macOS EDR scales absolute luminance with the brightness setting); on Linux and Android, where `DisplayCaps.sdr_white_nits` is known, additionally 1000 ± 50 nits absolute. |
| **FR-HDR-004** | `VideoSurface` MUST offer the tone-mapping functions `Auto`, `BT2390`, `BT2446A`, `Spline`, `Clip` and `None` (`ovt::render::ToneMap`), the gamut-mapping modes `perceptual`, `relative`, `saturation`, `clip`, and MUST expose the selected function and the display peak in the status bar. | §10.2.3, §10.3.4, WI-1.403 | Given `pq_patches_1000.mp4` and `ToneMap::Clip` with `clip_nits = 500`, When rendered to `Rgba8Srgb`, Then the 1000-nit and 4000-nit patches have identical RGB. |
| **FR-HDR-006** | `VideoSurface::measure()` MUST compute per frame in linear light: MaxCLL (max over samples of max(R,G,B) in cd/m²), FALL (mean of max(R,G,B)), the 99.99th percentile luminance, and MUST accumulate MaxFALL over the frames viewed, using the PQ EOTF (ST 2084) or HLG inverse OETF + OOTF, with the result shown in the HDR panel and exported by `ovt hdr --measure` (chapter 12 §12.2.11). | §10.7, WI-4.452, WI-4.457 | Given `pq_patches_1000.mp4` frame 0 (patches at 1/100/203/1000/4000/10 000 nits), When `measure()` returns valid, Then `max_cll_nits` = 10 000 ± 1 % and `fall_nits` equals the area-weighted mean of the patch nits in `pq_patches_1000.expected.json` ± 1 %. |
| **FR-HDR-007** | When the display is SDR, OVT MUST still accept HDR input and MUST label the picture region with a persistent, non-modal badge `Tone-mapped (BT.2390, 203 → 100 nit)` naming the function and the mapping so that users never mistake tone-mapped output for true HDR. | §10.1.2 (EMBEDDED mode), §10.3.4; badge drawn by chapter 11 §11.3.2 | Given an SDR display and a PQ stream, When rendered, Then `active_encoding() == Srgb8` and `DisplayCaps.hdr_active == false` (the GUI derives the badge from these). |
| **FR-HDR-008** | Dithering to the output bit depth MUST be applied in `VideoSurface` (blue-noise, libplacebo `pl_dither_params`) for every output, and the output format MUST be 10-bit or FP16 on HDR surfaces (macOS: `MTLPixelFormatRGBA16Float` with `extendedLinearSRGB` or `bgr10a2Unorm` with ITU-R 2100 PQ; Android: `RGBA_1010102` / `RGBA_FP16` with `ADATASPACE_BT2020_PQ`/`BT2020_HLG`). | §10.3.2, §10.3.5, §10.4.1, §10.4.3 | Given a 10-bit grey ramp rendered to `Rgba8Srgb` with `dither = true` and `false`, When compared, Then the dithered output has no run of ≥ 8 identical consecutive values along the ramp while the undithered one has. |
| **FR-HDR-010** | The picture region MUST offer HDR scopes as toggleable panels rendered from the linear-light texture: false colour with the bands `<1`, `1–100`, `100–203`, `203–1000`, `1000–4000`, `>4000` cd/m² and a legend; luminance waveform with a logarithmic nit axis from 0.0001 to 10 000; vectorscope in BT.2020 with BT.709 and P3 gamut triangles; gamut-clip overlay marking samples outside BT.709 (or P3, selectable); histogram in PQ code values. | §10.7.5–§10.7.8, WI-4.453, WI-4.454 | Given the PQ patches, When false colour is on, Then each patch is uniformly its band colour (golden `false_color_patches.png`). |
| **FR-HDR-011** | Static HDR metadata (ST 2086 primaries/white/luminance, CTA-861.3 MaxCLL/MaxFALL) MUST be passed to the display surface when it is HDR (macOS `CAEDRMetadata.hdr10`; Android `ANativeWindow` SMPTE2086/CTA861.3 metadata via `AHardwareBuffer`/EGL `EGL_EXT_surface_SMPTE2086_metadata`; Linux Wayland `image_description` in phase 4), and MUST be re-sent when the stream changes. | §10.3.6, §10.4.1, §10.4.3, WI-1.411 | Given a PQ source with MDCV 1000/0.005, When hinted, Then the `pl_color_space` passed to `pl_swapchain_colorspace_hint` has `hdr.max_luma == 1000`, `hdr.min_luma == 0.005`. |
| **FR-HDR-012** | HDR10+ (ST 2094-40) per-frame metadata, when present, MUST be passed to libplacebo as `pl_hdr_metadata.scene_max/scene_avg/ootf` so that tone mapping follows the signalled per-scene values, and the HDR panel MUST indicate `dynamic metadata in use`. | §10.3.6, WI-4.455 | Given the three ST 2094-40 sample payloads, When parsed, Then the fields equal FFmpeg's `AVDynamicHDRPlus` dump. |
| **FR-HDR-013** | Dolby Vision profile 5 and 8 RPUs MUST be parsed by the playback path (FFmpeg `AV_FRAME_DATA_DOVI_METADATA`) and applied by libplacebo to produce PQ output, and when the RPU cannot be applied the stream MUST still render as the base layer with diagnostic `DEC-HDR-0002` (Info; registry chapter 7 §7.9.4). | §10.3.7, WI-4.456 | Given the DV P8 corpus sample, When rendered to `SdrPng8` with BT2390, Then the mpv parity gate of §10.9.2 item 3 passes; given profile 7, Then `RND-0030` is raised once. |
| **FR-HDR-014** | Overlays (grids, arrows, labels, heat maps) MUST be composited at the SDR reference white (203 cd/m² on PQ/HLG surfaces, `sdr_white_nits` from `DisplayTarget`) so that overlay colours do not change brightness when the display switches between SDR and HDR modes. | §10.5, WI-2.431 | Given a PQ target, When the overlay layer contains white, Then the measurement texture reads 1.0 (= 203 nits) at that pixel. |
| **FR-PLN-001** | The picture region MUST offer plane modes `YUV` (colour-converted), `Y`, `U`, `V` and `Grey` (Y with the content transfer applied), selected by keys `1`–`4` and `5`, where `U`/`V` modes show the chroma plane upsampled to luma resolution with nearest-neighbour sampling and a neutral mid-grey mapping of the achromatic value (`128 << (bit_depth − 8)`). | §10.6.1, WI-1.408 | Given a 4:2:0 10-bit frame and plane mode U at zoom 32×, When rendered, Then each chroma sample is a 64×64 device-pixel square (chroma is half resolution) with its decimal value drawn centred. |
| **FR-PLN-002** | Plane views MUST operate on the decoded samples at native bit depth (8, 10 or 12) uploaded as `R8`/`R16` textures (ADR-005), MUST NOT round-trip through an 8-bit RGB conversion, and the GUI MUST show the sample value under the cursor in the status bar as `Y=<v> U=<v> V=<v>` (decimal or hex per the `H`/`D` toggle) with the 10/12-bit value, not a scaled 8-bit value. | §10.2.1, §10.3.1, §10.6.3, WI-1.401, WI-2.435 | Given a 10-bit frame with luma code 512 and `raw_code_values`, When plane mode Y is rendered to `Rgba8Srgb`, Then the output grey is 128 and `Pick.code[0] == 512`. |
| **FR-PLN-003** | When zoom ≥ 24 screen pixels per luma sample, the picture region MUST draw each sample's numeric value inside its square for the current plane (luma for `YUV`/`Y`/`Grey`; chroma at chroma resolution for `U`/`V`), in dec or hex per the `H`/`D` toggle, and MUST use nearest-neighbour magnification (no interpolation) at any zoom > 1×. | §10.6.2, WI-1.409, WI-2.433 | Given zoom 32 on an 8-bit frame, When rendered, Then golden `labels_zoom32.png` matches. |
| **FR-PLN-005** | The picture region MUST display any `PlaneStage` present in the current `FrameAnalysis` (`Reconstruction`, `Prediction`, `Residual`, `PreDeblock`, `PreSaoOrCdef`, `PreLoopRestoration`) with the same plane modes, where `Residual` is shown with a signed-to-unsigned offset of `1 << (bit_depth − 1)` and a user gain of 1–16×. | §10.2.1 (`Picture` built by the host from the `PlaneRef` blob), §10.6.1 | Given a `Residual` plane of all zeros at 10-bit, When shown as plane mode Y with gain 1, Then every output pixel is mid-grey (128). |
| **FR-PLN-007** | The picture region MUST support the compare modes of chapter 10 §10.2.4 between surface slots A and B (`VideoSurface::set_compare`): `Single`, `SideBySide`, `Split` (`H`/`V`, draggable divider), `Overlay`, `Difference` (\|A−B\| with gain 1–64× via `[`/`]`, optional per-plane selection and false-colour mapping) and `Flicker`; `C`/`Shift+C` cycle the mode and `X` swaps A and B (chapter 11 §11.4.3). | §10.2.4, §10.6.4, WI-1.410 | Given A and B identical, When `Difference` with gain 32, Then all output pixels are 0 and `region_psnr[0]` is reported as 99.0. The `CompareMode` enum of §10.2.4 is the normative vocabulary (`Split` + `split_axis` = SplitH/SplitV, `Flicker` = Alternate, `Difference` = Subtraction, `Difference` + `diff_false_color` = Temperature; `Checkerboard` is not offered) |
| **FR-PLN-010** | Frame grabs MUST be exportable as PNG (8-bit, display-referred as rendered) and as 16-bit PNG or raw plane dumps (`.yuv`, planar, native bit depth, little-endian) of the current frame and plane stage, and `ovt dump --frames N --output frame.yuv` MUST produce byte-identical plane dumps. | §10.8, WI-1.412 | Given `pq_patches_1000.mp4`, When exported as `HdrPng16`, Then the PNG has `cICP = 09 10 00 01` and the 1000-nit patch value is within ±32/65535 of 0.7518. |
| **FR-BLK-017** | `ovt-studio` MUST render these overlays from `FrameAnalysis` on the picture region, each toggleable independently and selectable as an F-key mode (chapter 3 UX-2): partition boundaries by level (distinct line widths for CTU/SB, CU/block, PU, TU/TXB), slice/tile boundaries, prediction kind fill (intra/inter/skip/intrabc/palette), intra direction glyphs, MV arrows (L0 and L1 in distinct colours, scaled by zoom with a user gain 0.25–8×), QP/qindex heat map, bits heat map, reference-index map, transform type/size, deblocking boundary strength, SAO/CDEF index, and block labels (`I`/`P`/`B`/`S`, mode names) when zoom ≥ 4×. | §10.5, WI-2.430–WI-2.437 | Given `tests/render/fixtures/synthetic_4k_grid.fb`, When `set_overlays` and `render` are called, Then golden `synthetic_4k_grid.png` matches with ≤ 0.1 % differing pixels. |
| **FR-BLK-018** | Overlay redraw of a 4K frame with MV arrows for every 4×4 cell (≈ 518 400 arrows) MUST complete in ≤ 16 ms on the M1 reference, and panning/zooming MUST not trigger a rebuild of `OverlaySet` GPU buffers. | §10.5.4, §10.5.8, WI-2.434, WI-2.437, WI-2.438 | Given `bench_overlay_4k_mv_arrows` with its chapter 14 §14.9.3 input, When run on `m1-air-8gb` and on `pixel-tablet` (1080p input), Then p95 frame time meets the §14.9.3 budget of each device (the budget values are owned by chapter 14, D19). |
| **FR-GUI-030** | `ovt-studio` MUST render the picture region through `libovt-render`'s `VideoSurface` embedded in a `QQuickRhiItem`/`QSGRenderNode` (ADR-004) using Vulkan (MoltenVK on macOS, Vulkan or GLES on Android, Vulkan on Linux), and MUST NOT draw video or overlays with Qt Quick scene-graph primitives. | §10.1.2, §10.2.2, WI-1.404, WI-1.405 | Given the app showing `sdr709_ramp.mp4`, When the `PictureHost` item is screenshotted, Then it matches `SdrPng8` of frame 0 (≤ 0.1 % pixels differ). |
| **NFR-003** | Overlay redraw at 4K with MV arrows on every 4×4 cell MUST take ≤ 16 ms (95th percentile, as measured by `bench_overlay_*`; chapter 16 §16.6). | §10.5.8, WI-2.438 | As FR-BLK-018 (chapter 14 §14.9.3 records p95 and p99). |
| **FR-HDR-101** | All UI chrome MUST stay SDR; only the video surface is HDR (chapter 10 §10.1.2 rules R-ARCH-1 and R-ARCH-2). | §10.1.2 R-ARCH-1, R-ARCH-2 | Given HDR mode on, When a screenshot of the window is taken, Then no pixel outside the picture panel exceeds SDR white. |
| **FR-PLN-101** | Rendering MUST be deterministic: the same inputs and `ViewState` MUST produce byte-identical offscreen output on the same GPU and driver, so peak detection and any frame-history-dependent tone mapping MUST be off for offscreen, export and test renders (chapter 10 §10.3.4 rule R-TM-1; replaces the retired `NFR-41x` ids of earlier chapter-10 drafts). | §10.3.4 R-TM-1, §10.9.2 item 5 | Given any fixture, When rendered twice to an offscreen RGBA16F target, Then the two downloads are byte-identical. |

The overlay budget is FR-BLK-018 / NFR-003; the determinism rule is FR-PLN-101.

### 10.1.2 "SDR chrome + HDR video surface"

No toolkit composites SDR UI and HDR video correctly on all three platforms (Appendix B.4). OVT therefore uses one renderer (`libovt-render`) behind one interface (`VideoSurface`) with two **presentation modes**:

```
+--------------------------------------------------------------------------+
| Qt Quick window (always SDR, 8-bit sRGB swapchain)                       |
|                                                                          |
|  +--------------- Picture panel (QML item "PictureHost") -------------+  |
|  |  Mode EMBEDDED: QQuickRhiItem shows the texture libplacebo wrote    |  |
|  |      (RGBA8 sRGB, tone-mapped) — default, works everywhere          |  |
|  |  Mode NATIVE:   a child native surface covers exactly this rect     |  |
|  |      macOS   -> child QWindow(MetalSurface) -> CAMetalLayer, EDR    |  |
|  |      Linux   -> child QWindow(VulkanSurface) -> wl_subsurface       |  |
|  |      Android -> SurfaceView sibling (Kotlin shell), PQ/HLG dataspace|  |
|  |      libplacebo renders video + overlays into it                    |  |
|  +--------------------------------------------------------------------+  |
+--------------------------------------------------------------------------+
```

Rules:

1. **R-ARCH-1.** In both modes the *same* `VideoSurface` object does the drawing; only `DisplayTarget` differs. Overlays are always drawn by `libovt-render` in the same pass as the video (never by QML items), so that the two modes are pixel-identical apart from colour volume.
2. **R-ARCH-2.** NATIVE mode is entered only when the display is HDR-capable (§10.4.5) *and* the user turns on "HDR output" (Chapter 11 §11.3.2). Otherwise EMBEDDED is used. Content type never forces a mode.
3. **R-ARCH-3.** The EMBEDDED texture is kept up to date even in NATIVE mode (rendered on demand when the native surface is hidden), so that a popup covering the picture area can hide the native surface and show the embedded picture instead (Chapter 11 §11.5.5).
4. **R-ARCH-4.** `libovt-render` has no Qt, Android-Java or AppKit types in its public headers. Platform glue lives in `render/src/platform/` behind `#ifdef`s and receives raw handles (`CAMetalLayer*`, `ANativeWindow*`, `wl_display*`/`wl_surface*`, `VkInstance`/`VkSurfaceKHR`).
5. **R-ARCH-5.** libplacebo is used through its Vulkan backend on macOS (MoltenVK) and Linux, and through its OpenGL ES backend on Android (ADR-014). A Vulkan-on-Android path (`AHardwareBuffer`, §10.4.3) is an optional later optimisation.

### 10.1.3 Source layout

```
render/
  include/ovt/render/video_surface.h   VideoSurface, DisplayTarget, ViewState, CompareMode, Pick, Measurements
  include/ovt/render/picture.h         Picture, PlaneData
  include/ovt/render/overlay_set.h     OverlaySet, OverlayBuilder, OverlayConfig, colour tables
  include/ovt/render/display_caps.h    DisplayCaps, display_probe()
  include/ovt/render/export.h          export_png(), ExportKind
  src/surface/video_surface_impl.cpp   the one concrete VideoSurface
  src/color/h273_map.cpp               ovt_ColorDescription -> pl_color_repr / pl_color_space (§10.3.3)
  src/color/hdr10plus_map.cpp          ST 2094-40 -> pl_hdr_metadata (§10.3.6)
  src/pipeline/frame_upload.cpp        Picture -> pl_frame / pl_plane_data (§10.3.1)
  src/pipeline/compare.cpp             compare-mode shaders (§10.6.4)
  src/overlay/*.cpp                    builders + passes: grid, arrows, heatmap, labels, idbuffer (§10.5)
  src/measure/*.cpp                    nits conversion, reductions, scopes (§10.7)
  src/platform/macos_metal.mm          CAMetalLayer setup, EDR polling, CAEDRMetadata
  src/platform/linux_wayland.cpp       wp_color_manager_v1 probe, VkSurface creation
  src/platform/android_egl.cpp         EGL + ANativeWindow dataspace
  src/platform/offscreen.cpp           OffscreenTexture targets + interop export
  src/gfx/pl_pass_helpers.cpp          thin helpers around pl_pass / pl_dispatch
  luts/*.h                             generated 256-entry LUTs (tools/gen_luts.py)
  fonts/dejavu_sans_mono_sdf.h         generated SDF atlas (tools/gen_sdf_atlas.py)
  tests/                               Catch2 tests, golden images manifest, fixtures (benchmarks live in tests/perf/render/, chapter 14 §14.9.3)
```

---

## 10.2 `libovt-render` public C++ interface

All declarations are in `namespace ovt::render`. Types from the core (`ovt_ColorDescription`, `FrameId`, `Rect`) are included from `ovt/color.h` and the generated `ovt_model_generated.h`. This section **expands §0.6.4**; §0.6.4 remains the normative shape and these are its full definitions.

### 10.2.1 `Picture`

```cpp
// render/include/ovt/render/picture.h
#pragma once
#include <cstdint>
#include <ovt/color.h>                    // ovt_ColorDescription
#include <ovt/model_generated.h>          // ovt::fb::FrameId, ovt::fb::Rect

namespace ovt::render {

struct PlaneData {
  const void* data = nullptr;   // first sample of row 0; MUST stay valid until set_frame() returns
  uint32_t width = 0;           // samples in this plane (chroma planes are subsampled)
  uint32_t height = 0;
  uint32_t stride_bytes = 0;    // >= width * sample_bytes; may include padding
  uint8_t  sample_bytes = 1;    // 1 for bit_depth <= 8, 2 for 9..16 (little-endian, LSB-aligned)
};

struct Picture {
  ovt::fb::FrameId id{};
  uint32_t coded_width = 0, coded_height = 0;   // luma samples before cropping
  ovt::fb::Rect crop{};                         // conformance window / render size (§0.1.9)
  uint8_t num_planes = 3;                       // 1 for 4:0:0, otherwise 3
  PlaneData planes[3];                          // 0=Y 1=U(Cb) 2=V(Cr)
  ovt_ColorDescription color{};                 // chroma_format, bit_depth, primaries, transfer, matrix, range, siting, static HDR
  bool interlaced = false, top_field_first = true;   // from FrameInfo (chapter 06); drives ViewState.field_mode (§10.6.1)
  // Dynamic metadata (nullable). Raw payloads as carried in the stream; the renderer parses them.
  const uint8_t* hdr10plus_payload = nullptr; size_t hdr10plus_len = 0;   // ST 2094-40 SEI payload (after itu_t_t35 header) / AV1 METADATA_TYPE_ITUT_T35
  const uint8_t* dovi_rpu = nullptr;          size_t dovi_rpu_len = 0;    // NOT raw RPU bytes despite the name (kept for ABI stability): the parsed Dolby Vision metadata as the libavutil `AVDOVIMetadata` side-data blob (`AV_FRAME_DATA_DOVI_METADATA`, §10.3.7); nullptr when the decoder exposes none
  // Monotonic generation counter. If two consecutive set_frame() calls carry the same generation for the same slot,
  // the surface MUST skip re-upload.
  uint64_t generation = 0;
};

} // namespace ovt::render
```

Rules: `planes[i].width` MUST equal `ceil(coded_width / sub_x)` where `sub_x` = 2 for 4:2:0/4:2:2 chroma and 1 otherwise; `planes[i].height` likewise with `sub_y` = 2 for 4:2:0. For `bit_depth` 9–16 the samples are stored in `uint16_t` with the value in the low `bit_depth` bits (as libavcodec, libde265, libaom and dav1d all do). A `Picture` with `bit_depth > 16` or `sample_bytes` not in {1, 2} MUST be rejected by `set_frame()` (returns `false`, logs `RND-0001`).

### 10.2.2 `DisplayTarget`

```cpp
// render/include/ovt/render/video_surface.h (part 1)
struct DisplayTarget {
  enum class Kind : uint8_t {
    OffscreenTexture,   // render into a texture the host composites (Qt QQuickRhiItem, CLI export, tests)
    MetalLayer,         // macOS: native = CAMetalLayer*
    AndroidWindow,      // Android: native = ANativeWindow* (from SurfaceView via ANativeWindow_fromSurface)
    WaylandSurface,     // Linux: native = wl_surface*, native2 = wl_display*
    VulkanSurface       // Any: native = VkSurfaceKHR, native2 = VkInstance (host created the surface, e.g. QVulkanInstance::surfaceForWindow)
  } kind = Kind::OffscreenTexture;
  void* native = nullptr;
  void* native2 = nullptr;
  int width = 0, height = 0;          // device pixels of the drawable
  float device_pixel_ratio = 1.0f;    // for label sizing; 2.0 on Retina
  // Display description (from display_probe(), §10.4.5). The surface treats these as the current output.
  bool  hdr_capable = false;
  float sdr_white_nits = 203.0f;      // what "SDR white" means on this output in nits (BT.2408 reference = 203)
  float headroom = 1.0f;              // max output luminance / sdr_white (1.0 = SDR). macOS: NSScreen EDR value
  enum class OutputEncoding : uint8_t { Srgb8, ScRgbLinear16F, Pq10, Hlg10 } encoding = OutputEncoding::Srgb8;
  // OffscreenTexture only:
  enum class OffscreenFormat : uint8_t { Rgba8Srgb, Rgba16F, Rgb10A2 } offscreen_format = OffscreenFormat::Rgba8Srgb;
  enum class Interop : uint8_t { None, MetalTexture, VulkanSharedDevice, GlTexture } interop = Interop::None;
  void* interop_device = nullptr;     // VulkanSharedDevice: pointer to struct VulkanDeviceInfo (instance, phys, device, queue family, queue index)
};

struct VulkanDeviceInfo {              // used when the host (Qt RHI, Vulkan backend) owns the device
  void* instance; void* physical_device; void* device; uint32_t queue_family; uint32_t queue_index;
  void* get_instance_proc_addr;       // PFN_vkGetInstanceProcAddr
};

struct ExternalTexture {               // how the host reads a texture the surface produced
  DisplayTarget::Interop interop; void* handle;   // id<MTLTexture> / VkImage / GLuint (cast)
  void* handle2;                      // VkImageView or 0
  int width, height; DisplayTarget::OffscreenFormat format; uint32_t layout; // VkImageLayout, 0 otherwise
};
```

Interop rules (one per platform, no alternatives in phase 1):

| Host | Qt RHI backend | Interop | Mechanism |
|---|---|---|---|
| macOS | Metal (Qt default) | `MetalTexture` | libplacebo exports the output `pl_tex` with `export_handle = PL_HANDLE_MTL_TEX` (MoltenVK `VK_EXT_metal_objects`); Qt wraps it via `QRhiTexture::createFrom({id<MTLTexture>, 0})`. Requires libplacebo built against MoltenVK ≥ 1.2.0; the handle type is declared in `libplacebo/gpu.h` (https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/gpu.h). |
| Linux | Vulkan (forced via `QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan)`) | `VulkanSharedDevice` | libplacebo imports Qt's device with `pl_vulkan_import()` (`libplacebo/vulkan.h`); the output `pl_tex` is wrapped by Qt via `QRhiTexture::createFrom({VkImage, layout})`. Synchronisation: both use the same `VkQueue`; the surface ends every render with `pl_gpu_flush()` and the host inserts a `QRhiResourceUpdateBatch` barrier; no cross-queue semaphores needed. |
| Android (EMBEDDED) | OpenGL ES (Qt default) | `GlTexture` | libplacebo `pl_opengl_create()` on a context *shared* with Qt's (`EGL_KHR_create_context` share group; created by the shell, §10.4.3); output `pl_tex` wrapped by Qt via `QRhiTexture::createFrom({GLuint, 0})`; sync via a fence: GLES 3.0 core `glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE)` on the surface's context after `pl_gpu_flush()`, and `glWaitSync` on Qt's context before sampling (fence objects are shared across contexts of one share group); `eglCreateSyncKHR`/`eglWaitSyncKHR` (`EGL_KHR_fence_sync`, `EGL_KHR_wait_sync`) when the two contexts are not in one share group; `glFinish()` only as the last-resort fallback behind `OVT_RENDER_GL_FINISH=1`. |
| Tests / CLI | none | `None` | CPU download via `pl_tex_download()`; the CLI and tests never share textures. |

### 10.2.3 `ViewState`, `ToneMap`, `GamutMap`

```cpp
enum class ToneMap : uint8_t { Auto, BT2390, BT2446A, Spline, Clip /* analysis: hard clip at clip_nits */, None /* pass-through */ };
enum class GamutMap : uint8_t { Perceptual, Relative, Saturation, Absolute, Desaturate, Darken, Clip, Highlight /* diagnostic */ };
enum class ChromaUpscale : uint8_t { Bilinear, Nearest };
enum class Downscale : uint8_t { Bilinear, Box, Lanczos };

struct ViewState {
  double zoom = 1.0;                  // device pixels per luma sample; 1.0 = pixel-exact; nearest-neighbour when >= 1.0
  double pan_x = 0, pan_y = 0;        // image coordinate (luma samples, coded-picture space) shown at the viewport centre
  int plane_mode = 0;                 // 0 YUV, 1 Y, 2 U, 3 V, 4 gray
  bool raw_code_values = false;       // plane modes 1-3: show code values without range expansion (engineering view)
  bool show_cropped = true;           // true: hide area outside crop (draw it dimmed at 30 %); false: show full coded picture
  ToneMap tone_map = ToneMap::Auto;
  GamutMap gamut_map = GamutMap::Perceptual;
  float clip_nits = 1000.0f;          // ToneMap::Clip threshold
  float target_peak_override = 0;     // 0 = use DisplayTarget.headroom*sdr_white; else force target peak (nits)
  bool false_color = false;           // §10.7.5
  bool gamut_clip_overlay = false;    // §10.7.8
  ChromaUpscale chroma_upscale = ChromaUpscale::Bilinear;
  Downscale downscale = Downscale::Bilinear;
  bool show_sample_values = true;     // text values when zoom >= 24
  bool dither = true;                 // disabled automatically for float targets
  uint32_t background_rgba = 0x202020FF;
  uint16_t rotation = 0;              // 0, 90, 180, 270 degrees clockwise; default from TrackInfo.rotation (§10.6.1)
  enum class FieldMode : uint8_t { Weave, Frame, TopField, BottomField } field_mode = FieldMode::Weave;   // interlaced pictures (§10.6.1)
  bool show_field_tint = false;       // FieldMode::Frame: tint odd rows to make field parity visible
};
```

Viewport mapping (normative, used by picking and the GUI): device pixel `(px, py)` ↔ image coordinate `(ix, iy)`:

```
ix = pan_x + (px - viewport_w/2) / zoom
iy = pan_y + (py - viewport_h/2) / zoom
```

`pan_x/pan_y` are clamped by the host so that at least 64 device pixels of picture stay visible.

### 10.2.4 `CompareMode`

```cpp
struct CompareMode {
  enum class Kind : uint8_t { Single, SideBySide, Split, Overlay, Difference, Flicker } kind = Kind::Single;
  enum class Axis : uint8_t { Vertical /* left|right */, Horizontal /* top/bottom */ } split_axis = Axis::Vertical;
  float split_position = 0.5f;        // Split: fraction of the viewport (0..1) where B begins
  float overlay_alpha = 0.5f;         // Overlay: B over A, alpha of B
  float diff_gain = 1.0f;             // Difference: |A-B| * gain (1, 2, 4, 8, 16, 32 in the GUI)
  bool  diff_false_color = false;     // Difference: map via turbo LUT instead of gray
  int8_t diff_plane = -1;             // Difference: -1 = max over planes; 0 Y, 1 U, 2 V
  bool  diff_signed = false;          // Difference: (A-B)*gain/2 + mid-grey instead of absolute
  uint16_t flicker_period_ms = 500;   // Flicker: A for period, then B for period
  bool  allow_scale_b = false;        // if B's dimensions differ from A's: false = error state RND-0010; true = resample B to A (bilinear)
};
```

### 10.2.5 `OverlaySet`, `OverlayBuilder`, `OverlayConfig`

```cpp
// render/include/ovt/render/overlay_set.h
enum class OverlayLayer : uint32_t {       // bitmask; draw order = ascending bit index (§10.5.6)
  Heatmap      = 1u << 0,
  PredFill     = 1u << 1,  // prediction-kind tint per block
  SkipHatch    = 1u << 2,
  GridTU       = 1u << 3,  GridPU = 1u << 4,  GridCU = 1u << 5,  GridCTU = 1u << 6,  // AV1: TXB/—/Block/SB; H.264: Tx/SubMB/—/MB
  GridPartition= 1u << 7,  // slice / tile boundaries
  MvL0 = 1u << 8, MvL1 = 1u << 9, MvBi = 1u << 10,
  DeblockEdges = 1u << 11,
  IntraDirs    = 1u << 12, // short direction stroke per intra block
  RefIdxFill   = 1u << 13, // colour per ref slot
  Selection    = 1u << 14,
  Labels       = 1u << 15,
  Probe        = 1u << 16, SelectionRect = 1u << 17
};

enum class HeatmapSource : uint8_t { None, QP, BitsX8, MvMagnitude, TxSize, FilterIdx, RefIdx, IntraMode, MetricMap /* from MetricsResult map blob */ , Custom /* host-provided per-cell u16 */ };
enum class Lut : uint8_t { Viridis, Turbo, Cividis, Jet, Hot };
enum class LabelContent : uint8_t { None, BlockSize, PredMode, QP, Bits, MvValues, IntraMode, TxType, RefIdx, BlockId };

struct OverlayConfig {
  uint32_t layers = 0;                              // OverlayLayer bits
  HeatmapSource heatmap = HeatmapSource::None; Lut lut = Lut::Viridis;
  float heatmap_alpha = 0.6f; float heatmap_min = 0, heatmap_max = 0;   // 0,0 = auto (min/max over the frame); bits are log2-scaled when auto
  LabelContent label = LabelContent::None;
  float label_zoom_threshold = 24.0f;              // device px per cell below which labels are not drawn
  float mv_scale = 1.0f;                           // arrow length multiplier (MVs are drawn in luma samples × mv_scale)
  float mv_min_px = 2.0f;                          // arrows shorter than this on screen are drawn as 2×2 dots
  float line_px = 1.0f;                            // grid line width in device pixels (scaled by device_pixel_ratio)
  uint32_t selected_block = 0xFFFFFFFFu; int32_t selected_mv = -1;
  ovt::fb::Rect selection_rect{};                  // zero w/h = none
  int probe_x = -1, probe_y = -1;                  // image coordinates of the pixel probe; -1 none
};

class OverlaySet;                                   // opaque; owns GPU buffers; created by a surface

class OverlayBuilder {
 public:
  explicit OverlayBuilder(VideoSurface& owner);
  // Builds GPU data for one frame. fa = FrameAnalysis root (verified FlatBuffer). custom_cells (optional) is cells_w*cells_h u16 values
  // for HeatmapSource::Custom or MetricMap (host converts a metric map blob to the cell grid before calling).
  // Thread-safe for the CPU part; GPU upload happens inside set_overlays().
  std::unique_ptr<OverlaySet> build(const ovt::fb::FrameAnalysis* fa, const OverlayConfig& cfg, const uint16_t* custom_cells = nullptr);
};
```

### 10.2.6 `Pick` and `Measurements`

```cpp
struct Pick {
  bool hit = false;                 // false when outside the picture
  int image_x = -1, image_y = -1;   // luma sample coordinates (coded picture)
  int cell_x = -1, cell_y = -1;     // image_x/4, image_y/4
  uint32_t block_id = 0xFFFFFFFFu;  // CellGrid.block_id at the cell (CPU lookup)
  int32_t mv_index = -1;            // index into FrameAnalysis.mvs if the cursor is on an arrow (GPU id buffer), else -1
  int32_t code[3] = {-1, -1, -1};   // Y, U, V code values of picture A at the sample (chroma: co-sited sample per siting)
  int32_t code_b[3] = {-1, -1, -1}; // picture B when a compare mode is active
  float rgb_linear[3] = {0, 0, 0};  // linear light, content primaries, 1.0 = 203 nits (libplacebo convention)
  float nits_luminance = -1;        // Y of CIE XYZ in cd/m² (-1 when unavailable)
  float nits_maxrgb = -1;           // max(R,G,B) in cd/m²
  float pq_code = -1;               // PQ-encoded max(R,G,B) in [0,1]
  float cie_x = 0, cie_y = 0;
};

struct ScopeTexture { ExternalTexture tex; bool valid; };

struct Measurements {
  bool valid = false;               // false until the first readback completes; results refer to measured_frame
  ovt::fb::FrameId measured_frame{};
  float max_cll_nits = 0;           // max over pixels of max(R,G,B) in nits (CTA-861.3 definition)
  float max_cll_p9999_nits = 0;     // 99.99th percentile of the same quantity (from the 1024-bin PQ histogram)
  float fall_nits = 0;              // mean over pixels of max(R,G,B) in nits (frame-average light level)
  float min_nits = 0;               // min over pixels of max(R,G,B)
  uint32_t histogram_pq[1024] = {}; // bin i covers PQ code [i/1024, (i+1)/1024) of max(R,G,B); counts are over *sampled* pixels (§10.7.3)
  uint32_t sampled_pixels = 0;      // number of pixels contributing (subsampling factor applied)
  uint32_t out_of_gamut_709 = 0, out_of_gamut_p3 = 0;   // sampled pixel counts outside BT.709 / P3 gamut (§10.7.8)
  float false_color_band_fraction[6] = {};              // fraction of pixels in each band of Table 10.7-1
  float region_psnr[3] = {-1, -1, -1};                  // Difference mode only: PSNR (dB) of the visible region per plane, code-value domain (§10.6.4)
  ScopeTexture waveform, vectorscope, cie_xy, histogram_image;
  double gpu_time_ms = 0;           // last render()+measure pass GPU time (0 if timers unavailable)
};
```

### 10.2.7 `VideoSurface`

```cpp
class VideoSurface {
 public:
  static std::unique_ptr<VideoSurface> create(const char* log_prefix = "render");
  virtual ~VideoSurface() = default;

  virtual bool init(const DisplayTarget&) = 0;              // creates pl_gpu, swapchain or offscreen target; false on failure (log RND-0002..)
  virtual bool resize(int width, int height) = 0;           // drawable size changed
  virtual void retarget(const DisplayTarget&) = 0;          // switch EMBEDDED <-> NATIVE; keeps uploaded frames
  virtual void set_frame(const Picture& a) = 0;             // slot A; uploads planes as R8/R16 textures
  virtual void set_compare(const Picture& b, const CompareMode&) = 0;   // slot B + mode; Kind::Single clears B
  virtual void set_overlays(std::unique_ptr<OverlaySet>) = 0;  // nullptr = no overlays
  virtual void render(const ViewState&) = 0;                // one frame; presents (native) or completes the texture (offscreen)
  virtual ExternalTexture output_texture() const = 0;       // OffscreenTexture targets only
  virtual Pick pick(int px, int py) const = 0;              // device pixels; uses last render()'s ViewState
  virtual Measurements measure() = 0;                       // non-blocking; see §10.7.9
  virtual bool export_png(const char* path, ExportKind, const ViewState* view_or_null) = 0;   // §10.8
  virtual DisplayTarget::OutputEncoding active_encoding() const = 0;
  virtual const char* last_error() const = 0;               // "" when none; stable code prefix "RND-nnnn"
};
```

Threading: a `VideoSurface` is confined to the thread that called `init()` (the render thread). `OverlayBuilder::build()` may run on any thread; `set_overlays()` on the render thread. `pick()` is cheap (≤ 50 µs; the id buffer readback is one 4-byte copy already completed for the last frame; §10.5.7).

---

## 10.3 libplacebo pipeline specification

libplacebo version pin: **≥ 7.349.0** (`third_party/VERSIONS.md`); API names below are from `libplacebo/renderer.h`, `utils/upload.h`, `colorspace.h`, `tone_mapping.h`, `gamut_mapping.h`, `shaders/dithering.h` (`pl_dither_params`, `PL_DITHER_BLUE_NOISE`; there is no `dither.h`) (https://github.com/haasn/libplacebo). The surface owns exactly one `pl_renderer` per target and one `pl_gpu`.

### 10.3.1 Filling `pl_frame` / `pl_plane_data`

For each `Picture` slot the surface keeps up to three `pl_tex` (R8 or R16 UNORM) and one `pl_frame`. Upload per plane `i`:

```c
struct pl_plane_data d = {
  .type = PL_FMT_UNORM,
  .width = pic.planes[i].width, .height = pic.planes[i].height,
  .pixel_stride = pic.planes[i].sample_bytes,
  .row_stride = pic.planes[i].stride_bytes,
  .component_size = { pic.planes[i].sample_bytes * 8 },   // 8 or 16
  .component_pad = { 0 },
  .component_map = { i },        // 0=Y, 1=Cb, 2=Cr
  .pixels = pic.planes[i].data,
};
pl_upload_plane(gpu, &frame.planes[i], &tex[i], &d);       // reallocates tex[i] when size/format changes
frame.num_planes = pic.num_planes;
frame.planes[i].components = 1;
frame.planes[i].component_mapping[0] = i;                  // PL_CHANNEL_Y / CB / CR
```

Bit depth is declared on the frame, not per plane:

```c
frame.repr.bits = (struct pl_bit_encoding){
  .sample_depth = pic.planes[0].sample_bytes * 8,   // 8 or 16
  .color_depth  = pic.color.bit_depth,              // 8, 10, 12, 16
  .bit_shift    = 0,                                // LSB-aligned storage (R-10.2.1)
};
```

libplacebo normalises `color_depth`-bit values stored in `sample_depth`-bit textures itself; the surface MUST NOT pre-shift samples. Chroma siting:

| `ovt_ColorDescription.chroma_siting` | `pl_chroma_location` |
|---|---|
| 0 unknown | `PL_CHROMA_LEFT` for H.264/HEVC (H.273 default `chroma_sample_loc_type_top_field = 0`) and for AV1 `CSP_UNKNOWN` (dav1d/libplacebo convention) |
| 1 left | `PL_CHROMA_LEFT` |
| 2 center | `PL_CHROMA_CENTER` |
| 3 top-left | `PL_CHROMA_TOP_LEFT` |

Apply with `pl_frame_set_chroma_location(&frame, loc)` after the planes are set (this writes `planes[1..2].shift_x/shift_y`). 4:2:2 and 4:4:4 use the same call; libplacebo derives the subsampling from texture sizes. 4:0:0: `num_planes = 1`, `repr.sys = PL_COLOR_SYSTEM_BT_709` is irrelevant; set `repr.sys` from the matrix anyway and libplacebo renders luma only (verified behaviour: monochrome frames render as grey).

Crop: `frame.crop = (pl_rect2df){ crop.x, crop.y, crop.x + crop.w, crop.y + crop.h }` when `ViewState.show_cropped`, else the full coded rect. Zoom/pan are expressed through `target.crop` (the destination rect in device pixels, may extend outside the drawable; libplacebo clips).

Scaling filters: `params.upscaler = zoom >= 1.0 ? &pl_filter_nearest : NULL`; `params.downscaler` = `&pl_filter_bilinear` / `&pl_filter_box` / `&pl_filter_lanczos` per `ViewState.downscale`; `params.plane_upscaler = chroma_upscale == Nearest ? &pl_filter_nearest : &pl_filter_bilinear`. `params.skip_anti_aliasing = true` when zoom ≥ 1 (pixel-exact view). `params.antiringing_strength = 0`.

### 10.3.2 Target frame

| `DisplayTarget.encoding` | Swapchain / texture format | `target.color` | `target.repr.bits.color_depth` | dither |
|---|---|---|---|---|
| `Srgb8` | `RGBA8` sRGB / `VK_FORMAT_B8G8R8A8_UNORM` + `SRGB_NONLINEAR` | primaries BT.709, transfer `PL_COLOR_TRC_SRGB`, `hdr.max_luma = 203` (= `PL_COLOR_SDR_WHITE`) | 8 | blue noise |
| `ScRgbLinear16F` (macOS EDR) | `RGBA16_SFLOAT` + `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` | primaries BT.709, transfer `PL_COLOR_TRC_LINEAR`, `hdr.max_luma = headroom × sdr_white_nits` | 16 (float → `dither_params = NULL`) | none |
| `Pq10` (Linux, Android, macOS "system PQ") | `A2B10G10R10_UNORM_PACK32` + `VK_COLOR_SPACE_HDR10_ST2084_EXT` / Android `ADATASPACE_BT2020_PQ` | primaries BT.2020, transfer `PL_COLOR_TRC_PQ`, `hdr.max_luma` = display max (DisplayCaps) or 1000 when unknown | 10 | blue noise |
| `Hlg10` (Android HLG-only displays) | `A2B10G10R10` + `ADATASPACE_BT2020_HLG` | primaries BT.2020, transfer `PL_COLOR_TRC_HLG`, `hdr.max_luma = 1000` | 10 | blue noise |

`pl_frame_from_swapchain(&target, &sw_frame)` fills format and size for native targets; for offscreen targets the surface sets `target.planes[0].texture` and `target.repr/color` from the table (`Rgba8Srgb` → row 1; `Rgba16F` → row 2; `Rgb10A2` → row 3).

### 10.3.3 `ovt_ColorDescription` → `pl_color_repr` / `pl_color_space`

Implemented in `render/src/color/h273_map.cpp` as pure functions with unit tests (`render/tests/test_h273_map.cpp`). Unknown codes map as shown and emit a log line `RND-0020 "unknown H.273 code N for <field>, assuming <value>"` once per stream.

**Primaries (H.273 Table 2)**

| code | meaning | `pl_color_primaries` |
|---|---|---|
| 1 | BT.709 / sRGB | `PL_COLOR_PRIM_BT_709` |
| 4 | BT.470 M | `PL_COLOR_PRIM_BT_470M` |
| 5 | BT.470 BG / BT.601-625 | `PL_COLOR_PRIM_BT_601_625` |
| 6, 7 | SMPTE 170M / 240M (601-525) | `PL_COLOR_PRIM_BT_601_525` |
| 8 | generic film (C) | `PL_COLOR_PRIM_FILM_C` |
| 9 | BT.2020 / BT.2100 | `PL_COLOR_PRIM_BT_2020` |
| 10 | CIE XYZ (ST 428-1) | `PL_COLOR_PRIM_CIE_1931` |
| 11 | DCI-P3 (ST 431-2) | `PL_COLOR_PRIM_DCI_P3` |
| 12 | Display P3 (ST 432-1) | `PL_COLOR_PRIM_DISPLAY_P3` |
| 22 | EBU Tech 3213-E | `PL_COLOR_PRIM_EBU_3213` |
| 0, 2, other | unspecified | `PL_COLOR_PRIM_UNKNOWN` → `pl_color_primaries_guess(width, height)` (≥ 1280 wide → 709, else 601-625/525 by height; libplacebo's rule) |

**Transfer (H.273 Table 3)**

| code | meaning | `pl_color_transfer` |
|---|---|---|
| 1, 6, 14, 15 | BT.709 / 601 / 2020 (10/12-bit) OETF → displayed via BT.1886 | `PL_COLOR_TRC_BT_1886` |
| 4 | gamma 2.2 | `PL_COLOR_TRC_GAMMA22` |
| 5 | gamma 2.8 | `PL_COLOR_TRC_GAMMA28` |
| 7 | SMPTE 240M | `PL_COLOR_TRC_BT_1886` |
| 8 | linear | `PL_COLOR_TRC_LINEAR` |
| 11, 12 | IEC 61966-2-4 (xvYCC), BT.1361 | `PL_COLOR_TRC_BT_1886` (+ `RND-0020`) |
| 13 | sRGB / sYCC | `PL_COLOR_TRC_SRGB` |
| 16 | PQ (ST 2084) | `PL_COLOR_TRC_PQ` |
| 17 | ST 428-1 | `PL_COLOR_TRC_ST428` |
| 18 | HLG (ARIB STD-B67) | `PL_COLOR_TRC_HLG` |
| 0, 2, other | unspecified | `PL_COLOR_TRC_UNKNOWN` → `PL_COLOR_TRC_BT_1886` |

**Matrix (H.273 Table 4)**

| code | meaning | `pl_color_system` |
|---|---|---|
| 0 | identity / RGB (GBR) | `PL_COLOR_SYSTEM_RGB` |
| 1 | BT.709 | `PL_COLOR_SYSTEM_BT_709` |
| 5, 6 | BT.470 BG / SMPTE 170M | `PL_COLOR_SYSTEM_BT_601` |
| 7 | SMPTE 240M | `PL_COLOR_SYSTEM_SMPTE_240M` |
| 8 | YCgCo | `PL_COLOR_SYSTEM_YCGCO` |
| 9 | BT.2020 non-constant luminance | `PL_COLOR_SYSTEM_BT_2020_NC` |
| 10 | BT.2020 constant luminance | `PL_COLOR_SYSTEM_BT_2020_C` |
| 14 | ICtCp | transfer 16 → `PL_COLOR_SYSTEM_BT_2100_PQ`; transfer 18 → `PL_COLOR_SYSTEM_BT_2100_HLG` |
| 12, 13 | chromaticity-derived NCL/CL | `PL_COLOR_SYSTEM_BT_709` + `Diagnostic` `RND-0021` (unsupported, rendered approximately) |
| 2, other | unspecified | `PL_COLOR_SYSTEM_UNKNOWN` → `pl_color_system_guess_ycbcr(width, height)` |

Dolby Vision overrides the matrix: when `color.has_dolby_vision && dovi_rpu` is present, `repr.sys = PL_COLOR_SYSTEM_DOLBYVISION` and `repr.dovi` points at a `pl_dovi_metadata` filled from the RPU (§10.3.7).

**Range and the rest**

```c
repr.levels = color.full_range ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
repr.alpha  = PL_ALPHA_NONE;
space.hdr.min_luma = color.mdcv_min_luminance ? color.mdcv_min_luminance * 1e-4f : 0;      // 0.0001 cd/m² units → nits
space.hdr.max_luma = color.mdcv_max_luminance ? color.mdcv_max_luminance * 1e-4f : 0;      // 0 → libplacebo infers (PQ: 10000, HLG: 1000, SDR: 203)
space.hdr.max_cll  = color.max_cll;  space.hdr.max_fall = color.max_fall;
for c in 0..2: space.hdr.prim.{red,green,blue}.{x,y} = color.mdcv_primaries[c][0..1] * 2e-5f;   // 0.00002 units → chromaticity
space.hdr.prim.white.{x,y} = color.mdcv_white_point[0..1] * 2e-5f;
```

MDCV primaries order differs per source: the H.265/H.264 `mastering_display_colour_volume` SEI (H.265 D.2.28) and the ISOBMFF `mdcv` box (ST 2086) carry **G, B, R**; the AV1 `metadata_hdr_mdcv()` OBU (AV1 §5.8.4/§6.7.4) carries **R, G, B** in 0.16 fixed point; FFmpeg `AVMasteringDisplayMetadata` is already R, G, B. **OVT rule (Chapter 7 §7.3.5 row 15 and §7.3.6 row 9, normative):** the neutral `ovt_ColorDescription.mdcv_primaries[3][2]` is **always R, G, B** (`[0]` red, `[1]` green, `[2]` blue, 0.00002 units) and every parser/plugin maps from its source order when filling it; the raw box view (the `display_primaries_x/y` `Field` children of the `mdcv` `BoxNode`, Chapter 8 §8.3 group C) keeps the box order unreordered. The map here therefore assigns `[0]→red, [1]→green, [2]→blue` with no reordering. Unit test: `test_h273_map.cpp/"mdcv order"` feeds the Chapter 7 WI-2.172 x265 PQ vector (`mdcv_primaries == {{34000,16000},{13250,34500},{7500,3000}}`) and expects `prim.red == (0.680, 0.320)`, `prim.green == (0.265, 0.690)`, `prim.blue == (0.150, 0.060)`.

### 10.3.4 Tone mapping and gamut mapping

`pl_render_params params = pl_render_default_params;` then `params.color_map_params = &cmap;` with:

| `ViewState.tone_map` | `cmap.tone_mapping_function` | other |
|---|---|---|
| `Auto` | `&pl_tone_map_spline` (libplacebo default) | `params.peak_detect_params = &pl_peak_detect_default_params` (scene-adaptive); `cmap.contrast_recovery = 0.3` |
| `BT2390` | `&pl_tone_map_bt2390` | `peak_detect_params = NULL` (static, deterministic) |
| `BT2446A` | `&pl_tone_map_bt2446a` | `peak_detect_params = NULL` |
| `Spline` | `&pl_tone_map_spline` | `peak_detect_params = NULL` |
| `Clip` | `&pl_tone_map_clip` | target `hdr.max_luma = ViewState.clip_nits`; everything above clips to display peak; used with false colour to find over-range content |
| `None` | `&pl_tone_map_clip` | target `hdr.max_luma` = source `hdr.max_luma` (pass-through; on an SDR display the swapchain clips) |

Rule R-TM-1: peak detection is **off** for everything except `Auto`, and off for all offscreen/export renders and in tests, because it makes output depend on previous frames (violates the determinism rule FR-PLN-101, §10.1.1).

`cmap.gamut_mapping` = `&pl_gamut_map_perceptual` / `_relative` / `_saturation` / `_absolute` / `_desaturate` / `_darken` / `_clip` / `_highlight` per `ViewState.gamut_map` (names from `libplacebo/gamut_mapping.h`). `Highlight` paints out-of-gamut pixels and is the implementation of the "gamut clip overlay" when `ViewState.gamut_clip_overlay` is true (§10.7.8).

`cmap.metadata = PL_HDR_METADATA_ANY` (use HDR10+ scene metadata when present, else static, else peak detection).

### 10.3.5 Dithering

`params.dither_params = &dither;` with `dither = pl_dither_default_params; dither.method = PL_DITHER_BLUE_NOISE; dither.lut_size = 6; dither.temporal = false;` The target depth is taken from `target.repr.bits.color_depth` (8 or 10). For float targets `params.dither_params = NULL`. `ViewState.dither = false` sets it to NULL for all targets (shows banding deliberately — useful to inspect source banding).

### 10.3.6 HDR metadata output

After `init()` and whenever the source colour space changes, the surface calls `pl_swapchain_colorspace_hint(sw, &hint)` with `hint` = the target `pl_color_space` of §10.3.2 whose `hdr` block is filled with the **content's** static metadata (max/min luma, MDCV primaries, MaxCLL/MaxFALL) when `encoding == Pq10/Hlg10`. libplacebo forwards it with `vkSetHdrMetadataEXT` (Vulkan), and on Android the EGL path (§10.4.3) sets `EGL_EXT_surface_SMPTE2086_metadata` / `EGL_EXT_surface_CTA861_3_metadata` attributes itself, because libplacebo's GL swapchain carries no metadata.

HDR10+ (ST 2094-40, `Picture.hdr10plus_payload`): parsed by `render/src/color/hdr10plus_map.cpp` (own parser over `BitReader`, fuzz target `fuzz_hdr10plus`) into `frame.color.hdr` dynamic fields exactly as libplacebo's `pl_frame_from_avframe` does for `AV_FRAME_DATA_DYNAMIC_HDR_PLUS` (reference: `src/include/libplacebo/utils/libav_internal.h` in the libplacebo tree): processing window 0 only; `scene_max[c] = maxscl[c]` (nits), `scene_avg = average_maxrgb` (nits), `ootf.target_luma = targeted_system_display_maximum_luminance`, `ootf.knee_x/knee_y` from the Bezier knee point, `ootf.anchors[]` from the Bezier anchors, `ootf.num_anchors`. With `cmap.metadata = PL_HDR_METADATA_ANY` libplacebo uses them per scene. The HDR panel shows the raw fields (Chapter 11 §11.3.14).

### 10.3.7 Dolby Vision policy

1. RPU fields are **displayed** (Syntax tree and HDR panel) from the core's RPU parser (Chapter 7). For rendering, `render/src/color/dovi_map.cpp` consumes the **`AVDOVIMetadata`** (`libavutil/dovi_meta.h`, public) that the libavcodec playback decoder attaches as `AV_FRAME_DATA_DOVI_METADATA` side data; FFmpeg has no public standalone RPU parser (`ff_dovi_rpu_parse` is libavcodec-internal), so the core passes the side-data blob through `Picture::dovi_rpu` as the serialised `AVDOVIMetadata` (`av_dovi_metadata_alloc` layout, `dovi_rpu_len = size`) rather than the raw NAL payload. Pictures that did not come through libavcodec (analysis-path decoders) carry `dovi_rpu = nullptr` and render as base layer with `RND-0031` absent. `dovi_map.cpp` converts the struct with libplacebo's `pl_map_dovi_metadata()` (`libplacebo/utils/libav.h`) into `pl_dovi_metadata`; it is the only file in `render/` allowed to include libavutil headers.
2. Profiles **5** (IPT-PQ-c2, no base layer compatibility) and **8** (PQ/HLG/SDR-compatible base layer) are **reshaped to PQ** by libplacebo (`PL_COLOR_SYSTEM_DOLBYVISION` + `repr.dovi`), as mpv/VLC do. Profile 7 enhancement layers (dual-layer) are **not** composed; the base layer is shown and `Diagnostic RND-0030 "DV profile 7 EL ignored"` is raised. Profile 4/other → base layer only.
3. The GUI MUST show the label **"Dolby Vision: RPU-reshaped PQ rendering — not Dolby-certified"** (`qsTr`) in the Picture panel's status strip whenever `has_dolby_vision` is set, and the HDR panel repeats it. OVT never claims to show the Dolby display-management result.
4. No Dolby SDK, no `libdovi` dependency (its licence is MIT but it is Rust; the FFmpeg parser suffices).

---

## 10.4 Per-platform HDR output paths

### 10.4.1 macOS (EDR via CAMetalLayer, libplacebo on MoltenVK)

**Decision (binding, from the task brief):** the video surface uses **libplacebo → Vulkan → MoltenVK → CAMetalLayer**; Qt keeps its default Metal backend and an SDR swapchain. Qt's own `QRhiSwapChain::HDRExtendedSrgbLinear` is **not** used, because (a) it would make the whole QQuickWindow HDR and all QML chrome would need SDR-white scaling (Appendix B.4, QTBUG-134590), (b) libplacebo's MoltenVK path is the one mpv ships and tests (https://github.com/mpv-player/mpv/discussions/14138).

Native target creation (app side, Chapter 11 §11.2.1): the Picture panel creates a child `QWindow` with `setSurfaceType(QSurface::MetalSurface)`, parented to the main `QQuickWindow`, geometry = panel rect in device-independent pixels. `app/src/platform/mac_layer.mm` obtains the layer:

```objc
NSView* view = reinterpret_cast<NSView*>(window->winId());
CAMetalLayer* layer = (CAMetalLayer*)view.layer;          // Qt creates a CAMetalLayer for MetalSurface windows
```

and passes `DisplayTarget{Kind::MetalLayer, layer, nullptr, w*dpr, h*dpr, dpr, caps...}`. `render/src/platform/macos_metal.mm` then:

```objc
layer.wantsExtendedDynamicRangeContent = YES;                 // EDR on
layer.pixelFormat = (enc == ScRgbLinear16F) ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGR10A2Unorm;
layer.colorspace  = (enc == ScRgbLinear16F) ? CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB)
                  : (enc == Pq10)           ? CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ)
                                            : CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_HLG);
layer.edrMetadata = (enc == Pq10) ? [CAEDRMetadata HDR10MetadataWithMinLuminance:min_nits maxLuminance:max_nits opticalOutputScale:10000]
                  : (enc == Hlg10) ? [CAEDRMetadata HLGMetadata] : nil;   // scRGB: none; we tone-map ourselves
layer.displaySyncEnabled = YES;
```

(Apple docs: https://developer.apple.com/documentation/metal/using-color-spaces-to-display-hdr-content, https://developer.apple.com/documentation/quartzcore/caedrmetadata.) Then `vkCreateMetalSurfaceEXT(instance, &(VkMetalSurfaceCreateInfoEXT){ .pLayer = layer }, …)` and `pl_vulkan_create_swapchain(vk, &(struct pl_vulkan_swapchain_params){ .surface = surf, .present_mode = VK_PRESENT_MODE_FIFO_KHR, .surface_format = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT } })`. MoltenVK also sets the layer colourspace from the requested `VkColorSpaceKHR`; the explicit assignments above are belt-and-braces and make the "system PQ" option possible.

Default encoding on macOS = **`ScRgbLinear16F`** (libplacebo tone-maps to `headroom × sdr_white`, 1.0 = SDR white; deterministic, same code path as Linux/Android apart from the target). Option **"System PQ pass-through"** = `Pq10` + `kCGColorSpaceITUR_2100_PQ` + `CAEDRMetadata.hdr10` with `ToneMap::None`, for checking what macOS does with the signal. `sdr_white_nits` is not exposed by macOS (EDR is relative to the user's brightness); it is reported as "relative" in the Display panel and nits in `Pick`/`Measurements` are content-side values (always true: measurements are computed from the source, §10.7).

Headroom polling: `[NSScreen maximumExtendedDynamicRangeColorComponentValue]` read on `NSApplicationDidChangeScreenParametersNotification`, on window-screen change, and every **500 ms** by a timer while NATIVE mode is active (the value changes with ambient brightness and with HDR content appearing elsewhere). `maximumPotentialExtendedDynamicRangeColorComponentValue` > 1.0 defines `hdr_capable`. A change > 5 % triggers `retarget()` with the new `headroom` (libplacebo re-derives the tone curve; no swapchain recreation).

### 10.4.2 Linux (Vulkan + Wayland `color-management-v1`; X11 → SDR)

Requirements: Wayland session; compositor with `wp_color_manager_v1`: **KWin ≥ 6.4** (Plasma 6.4 ships the final v1 protocol; earlier Plasma 6.x used the `xx-` drafts), **Mutter ≥ 48** (GNOME 48), wlroots ≥ 0.19 / Sway 1.12 (Vulkan renderer); **Mesa ≥ 25.1** Vulkan WSI (exposes `VK_EXT_swapchain_colorspace` HDR spaces and `VK_EXT_hdr_metadata` over Wayland when the compositor supports the protocol: https://www.phoronix.com/news/Mesa-Vulkan-WSI-HDR-CM, protocol: https://www.phoronix.com/news/Wayland-CM-HDR-Merged). NVIDIA proprietary and X11/Xwayland: `hdr_capable = false`, reason string shown in the Display panel.

Target creation: the Picture panel creates a child `QWindow` with `setSurfaceType(QSurface::VulkanSurface)` and `setVulkanInstance(ovtVulkanInstance)` where `ovtVulkanInstance` is a `QVulkanInstance` created by the app with extensions `VK_KHR_surface`, `VK_KHR_wayland_surface`, `VK_EXT_swapchain_colorspace`, `VK_KHR_get_surface_capabilities2`. Qt Wayland implements child windows as `wl_subsurface`s (desynchronised), so the child moves/resizes with the panel without protocol work in OVT. The app obtains `VkSurfaceKHR surf = QVulkanInstance::surfaceForWindow(child)` and passes `DisplayTarget{Kind::VulkanSurface, (void*)surf, (void*)vkInstance, …}`. The surface then enumerates `vkGetPhysicalDeviceSurfaceFormatsKHR`; HDR is available iff the list contains `{A2B10G10R10_UNORM_PACK32, HDR10_ST2084_EXT}` (preferred → `Pq10`) or `{R16G16B16A16_SFLOAT, EXTENDED_SRGB_LINEAR_EXT}` (→ `ScRgbLinear16F`). Otherwise `init()` falls back to `Srgb8` and sets `last_error() = "RND-0040 no HDR surface format"`. Mesa's WSI performs the `wp_color_management_surface_v1` tagging; OVT MUST NOT create its own colour-management surface object for that `wl_surface` (a surface may have only one; protocol error otherwise). Qt's own Wayland colour-management client (a later Qt 6.x than the pinned 6.8.3 LTS, D5) only acts on windows that request an HDR `QRhiSwapChain` format, which OVT never does.

Display luminance (for `DisplayCaps`): `render/src/platform/linux_wayland.cpp` binds its own `wl_registry` on the `wl_display*` from `qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()->display()` (public API since Qt 6.5), binds `wp_color_manager_v1` and every `wl_output` (v4, with `name` events), calls `wp_color_manager_v1_get_output()` → `wp_color_management_output_v1_get_image_description()` → `wp_image_description_v1_get_information()` and reads the events `primaries`, `target_primaries`, `target_luminance (min, max)`, `target_max_cll`, `target_max_fall`. Outputs are matched to `QScreen` by `wl_output.name == QScreen::name()`. If the manager is absent: `hdr_capable=false, reason="compositor lacks wp_color_manager_v1"`.

### 10.4.3 Android (SurfaceView + dataspace, EGL path; ADR-014)

The Kotlin shell (Chapter 11 §11.5) owns a `SurfaceView` sibling of the `QtQuickView`, `setZOrderMediaOverlay(true)`, positioned at the Picture panel rect. In `surfaceCreated`, the shell passes the `Surface` to JNI; `render/src/platform/android_egl.cpp` does:

```c
ANativeWindow* win = ANativeWindow_fromSurface(env, jsurface);
// 1. Dataspace BEFORE the EGL surface exists (SurfaceFlinger reads it at buffer queue setup)
int32_t ds = (enc == Pq10) ? 163971072 /* ADATASPACE_BT2020_PQ  = STANDARD_BT2020|TRANSFER_ST2084|RANGE_FULL */
           : (enc == Hlg10)? 168165376 /* ADATASPACE_BT2020_HLG = STANDARD_BT2020|TRANSFER_HLG|RANGE_FULL */
           : 0 /* ADATASPACE_UNKNOWN -> sRGB */;
int rc = ANativeWindow_setBuffersDataSpace(win, ds);           // <android/native_window.h>, API 28; 0 on success
// 2. EGL config: RGB10A2 for PQ/HLG, RGBA8 for SDR
EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                     EGL_RED_SIZE, 10, EGL_GREEN_SIZE, 10, EGL_BLUE_SIZE, 10, EGL_ALPHA_SIZE, 2, EGL_NONE };
// 3. Surface attribs: if EGL_EXT_gl_colorspace_bt2020_pq / _hlg are present, also tag the EGL surface
EGLint sattr[] = { EGL_GL_COLORSPACE_KHR, (enc == Pq10) ? EGL_GL_COLORSPACE_BT2020_PQ_EXT : EGL_GL_COLORSPACE_BT2020_HLG_EXT, EGL_NONE };
EGLSurface s = eglCreateWindowSurface(dpy, cfg, win, has_ext ? sattr : NULL);
// 4. Static metadata (EGL_EXT_surface_SMPTE2086_metadata, EGL_EXT_surface_CTA861_3_metadata): EVERY attribute is the real value × EGL_METADATA_SCALING_EXT (= 50000)
eglSurfaceAttrib(dpy, s, EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, (EGLint)(prim.red.x * EGL_METADATA_SCALING_EXT)); /* ... GX,GY,BX,BY,WX,WY */
eglSurfaceAttrib(dpy, s, EGL_SMPTE2086_MAX_LUMINANCE_EXT, (EGLint)(max_nits * EGL_METADATA_SCALING_EXT)); eglSurfaceAttrib(dpy, s, EGL_SMPTE2086_MIN_LUMINANCE_EXT, (EGLint)(min_nits * EGL_METADATA_SCALING_EXT));
eglSurfaceAttrib(dpy, s, EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT, (EGLint)(max_cll * EGL_METADATA_SCALING_EXT)); eglSurfaceAttrib(dpy, s, EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT, (EGLint)(max_fall * EGL_METADATA_SCALING_EXT));
// 5. libplacebo on this context
pl_opengl gl = pl_opengl_create(log, &(struct pl_opengl_params){ .egl_display = dpy, .egl_context = ctx, .make_current = ..., .release_current = ... });
pl_swapchain sw = pl_opengl_create_swapchain(gl, &(struct pl_opengl_swapchain_params){ .swap_buffers = ovt_egl_swap, .framebuffer = { .id = 0, .flipped = false }, .max_swapchain_depth = 2 });
```

The numeric dataspace constants are those of `<android/data_space.h>`; the implementer MUST compile against the NDK header names and keep the literal values only in the unit test `test_android_dataspace.cpp` as a guard. `ANativeWindow_setBuffersDataSpace` returning non-zero → `hdr_capable=false` for this session and `RND-0050`. Android HDR dataspaces are full range by definition; libplacebo's PQ target (`PL_COLOR_LEVELS_FULL`, set by `pl_frame_from_swapchain` for GL swapchains with our explicit `target.repr.levels = PL_COLOR_LEVELS_FULL`) matches.

Shell-side calls (Kotlin, `android/app/src/main/kotlin/org/ovt/HdrSurfaceView.kt`, WI-0.052/WI-A.400):

- `window.colorMode = ActivityInfo.COLOR_MODE_HDR` (API 26) before the surface is created.
- API ≥ 35: `surfaceView.setDesiredHdrHeadroom(2.0f)` in mixed SDR/HDR layout, `8.0f` in full-screen picture mode (Google's guidance: https://android-developers.googleblog.com/2025/09/hdr-and-user-interfaces.html).
- API ≥ 34: `display.isHdrSdrRatioAvailable` / `display.hdrSdrRatio` + `registerHdrSdrRatioChangedListener` → `headroom` → `retarget()`.
- Supported HDR types: `display.mode.supportedHdrTypes` (`Display.Mode.getSupportedHdrTypes()`, API ≥ 34); on API 28–33 `display.hdrCapabilities.supportedHdrTypes` (`Display.getHdrCapabilities()` is deprecated in API 34). Luminance: `display.hdrCapabilities.desiredMaxLuminance/desiredMaxAverageLuminance/desiredMinLuminance` (still the only API for these values) → `DisplayCaps` (https://developer.android.com/media/grow/hdr-playback).
- `TextureView` MUST NOT be used (API ≥ 33 forces SDR; ADR-014).

**Encoding choice on Android:** `Pq10` if `HDR_TYPE_HDR10` or `HDR10_PLUS` is supported; else `Hlg10` if `HDR_TYPE_HLG`; else `Srgb8` (EMBEDDED mode only). Source HLG on an HDR10-only display is converted to PQ by libplacebo (correct per BT.2100 with 1000-nit OOTF).

**`AHardwareBuffer` alternative (optional, phase A later):** Vulkan rendering into `AHardwareBuffer`-backed `VkImage`s (`VK_ANDROID_external_memory_android_hardware_buffer`), presented with `ASurfaceControl` + `ASurfaceTransaction_setBuffer` / `_setBufferDataSpace(ADATASPACE_BT2020_PQ)` / `_setHdrMetadata_smpte2086` / `_setHdrMetadata_cta861_3` (API 29). Chosen only if GLES performance misses the 33 ms Android budget (FR-BLK-018 / D5) on the Android reference device (chapter 14 §14.9.2) by > 2×; it keeps the dataspace path and avoids Vulkan swapchain colourspaces (unreliable per Arm: https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/54708/).

### 10.4.4 ChromeOS spike (WI-A.402) — procedure and pass/fail

Purpose: decide whether an Android `SurfaceView` tagged `BT2020_PQ` reaches an HDR Chromebook display as HDR through ARCVM/Exo (unverified; Appendix B.4).

Procedure (hardware: one HDR-capable Chromebook or Chromebook + HDR monitor over USB-C; a colorimeter if available):

1. Build `android/spike-hdr/` (a 200-line Kotlin + NDK app, no Qt): full-screen `SurfaceView`, dataspace PQ, RGB10A2, draws six patches at PQ code values for 1, 100, 203, 1000, 4000, 10000 nits. Code = `round(E' · (2^10 − 1))` for full range and `round(E' · 876 + 64)` for limited range, with `E' = ((c1 + c2·Y^m1) / (1 + c3·Y^m1))^m2`, `Y = L / 10000` (ST 2084 inverse EOTF, constants of §10.7.2): **full range 153, 520, 594, 769, 923, 1023; limited range 195, 509, 573, 723, 855, 940** (the generator of §10.9.2 computes them from the same formula and writes them to `*.expected.json`) plus a strip of system-UI white (an ordinary `TextView` background `#FFFFFF`).
2. Log: `Display.getMode().getSupportedHdrTypes()` (API 34+; `getHdrCapabilities().getSupportedHdrTypes()` below), `isHdr()`, `hdrSdrRatio` (API 34+), `ANativeWindow_setBuffersDataSpace` return code, `eglQueryString(EGL_EXTENSIONS)` (look for `EGL_EXT_gl_colorspace_bt2020_pq`), `adb shell dumpsys SurfaceFlinger | grep -i dataspace`.
3. Measure (colorimeter) or observe each patch.

Pass iff **all**: (a) `supportedHdrTypes` non-empty; (b) dataspace call returns 0; (c) the 1000-nit patch measures ≥ 2.0 × the system-UI white patch luminance (or, without a colorimeter, is unmistakably brighter than UI white and the 203-nit patch matches UI white within visual tolerance); (d) the 100-nit patch is darker than UI white. Fail → ChromeOS runs EMBEDDED SDR only, the Display panel says "HDR not passed through by ChromeOS", and the result (device, ChromeOS version, Android version, dataspace rc, measurements) is recorded in `docs/dev/chromeos-hdr-spike.md`. Partial (a,b pass; c fails) → file a ChromeOS bug and keep the SDR fallback.

### 10.4.5 Display-capability detection and the "Display capabilities" panel

```cpp
// render/include/ovt/render/display_caps.h
struct DisplayCaps {
  bool hdr_capable;                 // can enter NATIVE mode now
  bool hdr_active;                  // output is currently HDR (user setting / headroom > 1)
  float headroom;                   // current, 1.0 if SDR
  float potential_headroom;         // maximum the display can reach (macOS "potential"; others = max_nits/sdr_white)
  float sdr_white_nits;             // 203 unless the platform reports it; macOS: NaN ("relative")
  float max_nits, min_nits, max_fall_nits;   // 0 = unknown
  bool supports_pq, supports_hlg, supports_scrgb;
  bool wide_gamut;                  // primaries >= P3
  char primaries_name[16];          // "BT.709", "P3", "BT.2020", "unknown"
  char source[32];                  // "NSScreen", "wp_color_manager_v1", "Display.Mode.supportedHdrTypes", "none"
  char reason[96];                  // why not capable: "X11 session", "compositor lacks wp_color_manager_v1", "no HDR surface format", "HDR disabled in display settings", ...
  char platform_detail[128];        // e.g. "Mesa 25.1.3 radv; KWin 6.4.2", "Android 15 API 35; HDR10,HLG", "macOS 15.5; XDR"
};
DisplayCaps display_probe(const DisplayTarget& where /* kind + native identify the screen */);
```

Algorithm (one function per platform, same order):

1. Session check: X11 → not capable ("X11 session"). Android API < 26 → not capable.
2. Protocol/API availability: macOS `maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0`; Wayland `wp_color_manager_v1` bound and `wl_output` image description reports `target_luminance.max > 203`; Android `display.mode.supportedHdrTypes` non-empty (SDR panels such as the Pixel Tablet report an empty array → `hdr_capable = false`, `reason = "display has no HDR mode"`).
3. Surface-format check (desktop): create a probe `VkSurfaceKHR` (macOS: on a 1×1 hidden layer; Linux: the real child window) and require an HDR format pair (§10.4.2).
4. Luminance: macOS `headroom = maximumExtendedDynamicRangeColorComponentValue`; Linux `max_nits/min_nits/max_fall` from the image description, `headroom = max_nits / 203`; Android `max_nits = desiredMaxLuminance`, `headroom = hdrSdrRatio` (API 34+) else `max_nits / 203`.
5. `hdr_active = hdr_capable && headroom > 1.05`.

Re-probe triggers: screen change, 500 ms timer (macOS), `wp_image_description` change events (Linux), `HdrSdrRatioChangedListener` (Android), `onConfigurationChanged` (Android).

**Panel contents** (Chapter 11 §11.3.15 renders this table; one row per field): Screen name; HDR capable (yes/no + reason); HDR active; Output encoding in use (`Srgb8`/`ScRgbLinear16F`/`Pq10`/`Hlg10`); Headroom now / potential; SDR white (nits or "relative"); Max / min / max-FALL luminance (nits); Supported transfers (PQ, HLG, scRGB); Primaries; Source API; Platform detail; libplacebo version and `pl_gpu` name (`gpu->glsl`/driver string); last probe time; a button "Re-probe" and a button "Show test pattern" (renders `pq_patches_1000` into the surface).

---

## 10.5 Overlay rendering

Overlays are rendered by `libovt-render` into an **overlay layer**: an RGBA8 sRGB `pl_tex` the size of the drawable, cleared to transparent, drawn with the passes below, and handed to libplacebo as `pl_frame.overlays[0]` (`struct pl_overlay { .tex, .repr = {RGB, FULL, PL_ALPHA_PREMULTIPLIED}, .color = {BT_709, SRGB}, .mode = PL_OVERLAY_NORMAL, .parts = one full-size part }`). libplacebo blends it in linear light and maps its white to **SDR white (203 nits)** on HDR targets, which implements the SDR-white scaling rule R-OVL-1 without any overlay-shader knowledge of the target. (libplacebo's `pl_overlay` is the mechanism mpv uses for OSD/subtitles.) The **id buffer** (§10.5.7) is a second R32_UINT `pl_tex` drawn by the same passes.

All overlay passes are written with libplacebo's raw pass API (`pl_pass_create` / `pl_pass_run`, `libplacebo/gpu.h`), GLSL 3.30 / ES 3.00 compatible, so the same code runs on Vulkan, MoltenVK and GLES. Instancing is implemented by **vertex pulling**: the vertex buffer holds only a `uint8 corner` per vertex and the per-instance data lives in a texture read with `texelFetch(instances, ivec2((VID / K) % W, (VID / K) / W), 0)` where `VID` is `gl_VertexIndex` (Vulkan) or `gl_VertexID` (GL) — `pl_gpu->glsl.vulkan` selects the macro — and `K` is the vertex count per instance. This avoids SSBOs in vertex shaders (not guaranteed on GLES 3.1) and `pl_pass`'s lack of an instance count.

### 10.5.1 Data layouts

| Overlay | GPU data | Size at 4K (518,400 cells) | Rebuilt when |
|---|---|---|---|
| **Grid lines** (per level) | line list: `struct { float x, y; uint8 color_idx; uint8 pad[3]; }` (12 B/vertex), 8 vertices per block rect, generated **only for blocks intersecting the visible image rect and only for levels whose smallest block is ≥ 6 device px** | bounded by viewport: ≤ (viewport_px / 36) × 8 vertices ≈ 0.46 M vertices for a 1920×1080 viewport (5.5 MB) | frame, zoom crossing a level threshold, pan beyond the 25 % margin pre-fetched around the viewport |
| **Partition (slice/tile) boundaries** | same line list, colour `GridPartition`, 2 px, dashed via fragment `fract()` on a distance attribute | ≤ 4 k vertices | frame |
| **Pred-mode fill / skip hatch / ref-idx fill** | per-cell attribute texture `cells_w × cells_h` `RGBA8UI`: R = `PredKind`, G = `ref0+1`, B = `ref1+1`, A = flags (bit0 skip, bit1 merge, bit2 bi) | 960×540 × 4 B = 2 MB | frame |
| **Heatmap** | per-cell `R16_UNORM` texture (value normalised to `heatmap_min..max`), plus 5 static 256×1 `RGBA8` LUT textures | 960×540 × 2 B = 1 MB | frame, heatmap source/range change |
| **MV arrows** | instance texture `RGBA32F` + `RGBA32UI` pair, one texel per arrow: `{ox, oy, vx, vy}` (block centre and vector in luma samples) and `{color_idx \| flags<<8, id, 0, 0}`; 7 vertices per arrow (4 for the shaft quad, 3 for the head) | 518,400 texels × 32 B = 16.6 MB; 3.63 M vertices (`uint8` corner buffer 3.6 MB, static) | frame, `mv_scale` |
| **Intra directions** | reuse arrow pass with a 2-vertex stroke template | ≤ 518 k | frame |
| **Deblock edges** | line list from `deblock_bs_v/h`, colour by Bs (0 none, 1 weak, 2 strong) | ≤ 2 × cells × 2 vertices (24 MB worst case) → **only drawn when cells ≥ 8 device px**, i.e. ≤ 65 k edges per 1080p viewport | frame, zoom |
| **Labels** | glyph instance texture `RGBA32F`: `{x, y, glyph_u, glyph_v}` + `RGBA8`: `{scale, color_idx, 0, 0}`; 6 vertices per glyph; glyph atlas `R8` 1024×1024 SDF (DejaVu Sans Mono, 95 ASCII glyphs + `→ ↑ ↓ ← − ·`, 48 px source, 8 px spread, generated by `tools/gen_sdf_atlas.py` into `render/fonts/dejavu_sans_mono_sdf.h`) | labels exist only when a cell is ≥ 24 device px: ≤ 80×45 = 3,600 cells visible at 1080p → ≤ 36 k glyphs (10 chars) | frame, zoom, pan, label content |
| **Selection / probe / rect** | ≤ 64 vertices immediate | — | every render |
| **Id buffer** | `R32_UINT` drawable-size texture; cleared to 0 | 1920×1080 × 4 B = 8.3 MB | every render |

Cell attribute textures are indexed by image position: fragment shader computes `cell = ivec2(floor(image_xy / 4.0))` from the inverse view transform and uses `texelFetch` (nearest, no filtering).

LUT generation (`tools/gen_luts.py`, run at build time, output checked in): `viridis`, `cividis` from matplotlib's `_cm_listed.py` data (BSD); `turbo` from Google's polynomial approximation (Apache-2.0, https://gist.github.com/mikhailov-work/0d177465a8151eb6ede1768d51d476c7); `jet` and `hot` from matplotlib's segment definitions. Each is 256 RGBA8 entries, index 0 = minimum.

### 10.5.2 Colour conventions (normative RGBA hex, straight alpha; `render/include/ovt/render/overlay_colors.h` `kOverlayColors[]`, included by `overlay_set.h`)

| Index | Meaning | RGBA |
|---|---|---|
| 0 | Intra (`PredKind::Intra`) fill | `#2ECC40` A `0x66` (40 %) |
| 1 | Inter L0 / first ref (arrow, fill) | `#FF851B` A `0xFF` arrows; fill A `0x4D` |
| 2 | Inter L1 / second ref | `#B10DC9` A `0xFF`; fill A `0x4D` |
| 3 | Bi-prediction (both lists; `MotionVector.flags` bit0) | `#00E5FF` A `0xFF`; fill A `0x4D` (arrows for L0 and L1 of a bi block are still drawn in colours 1 and 2 unless layer `MvBi` is on, which draws both in cyan) |
| 4 | Skip (`PredKind::Skip`) | hatch: 45° lines, 4 device px period, `#AAAAAA` A `0x99` over transparent |
| 5 | IntraBC | `#20C997` A `0x66` |
| 6 | PCM / lossless | `#FFDC00` A `0x66` |
| 7 | Palette | `#F012BE` A `0x66` |
| 8 | Grid CTU / SB / MB | `#FFFFFF` A `0xE6` (2 px) |
| 9 | Grid CU / Block | `#FFFF00` A `0xCC` (1 px) |
| 10 | Grid PU / sub-MB | `#00FFFF` A `0xB3` (1 px) |
| 11 | Grid TU / TXB / transform 4×4-8×8 | `#FF00FF` A `0x99` (1 px) |
| 12 | Slice / tile boundary | `#FF4136` A `0xFF` (2 px dashed 8/4) |
| 13 | Deblock Bs = 1 | `#7FDBFF` A `0xCC` |
| 14 | Deblock Bs = 2 | `#0074D9` A `0xFF` |
| 15 | Selection outline | `#FFFFFF` A `0xFF` 2 px + outer `#000000` A `0xFF` 1 px |
| 16 | Hover outline | `#FFD700` A `0xFF` 1 px |
| 17 | Probe crosshair | `#FF4136` A `0xFF` 1 px |
| 18 | Selection rectangle | `#39CCCC` A `0xFF` 1 px, fill A `0x33` |
| 19 | Label text | `#FFFFFF` A `0xFF` with `#000000` A `0xCC` 1 px SDF outline |
| 20 | Intra direction stroke | `#2ECC40` A `0xFF` |
| 21 | Global / warped MV (flags bit2) | `#FFDC00` A `0xFF` |
| 22–29 | Ref slot 0–7 fills (`RefIdxFill`) | `#E6194B #3CB44B #FFE119 #4363D8 #F58231 #911EB4 #46F0F0 #F032E6`, A `0x66` |
| 30 | Frame type I (shared with GUI) | `#E53935` |
| 31 | Frame type P | `#1E88E5` |
| 32 | Frame type B | `#43A047` |

These values are also the single source for the GUI legend and timeline (Chapter 11 §11.4.5 imports them via `ovt::render::kOverlayColors`). Colour-blind-safe variant (`kOverlayColorsCvd[]`): L0 `#E69F00`, L1 `#56B4E9`, bi `#CC79A7`, intra `#009E73` (Okabe-Ito); heatmaps default to viridis.

The colour tables and the legend helper are render-library symbols (WI-2.439), **not** part of the core C API of chapter 12 §12.3 (`core/include/ovt/*.h`); the GUI links `libovt-render` and includes this header directly:

```cpp
// render/include/ovt/render/overlay_colors.h  (WI-2.439; included by overlay_set.h)
namespace ovt::render {
extern const std::array<uint32_t, 33> kOverlayColors;     // RGBA (straight alpha), index = row of the table above
extern const std::array<uint32_t, 33> kOverlayColorsCvd;  // colour-blind-safe variant; same indices
struct LegendEntry {
  uint8_t      color_index;   // row of the table above
  uint32_t     rgba;          // from the active palette
  OverlayLayer layer;         // the layer bit this entry explains
  const char*  label_key;     // translation key, e.g. "legend.inter_l0"; the GUI runs it through qsTr
};
}  // namespace ovt::render

/// Legend data for the GUI (chapter 11 §11.4.5). Fills `out` (capacity `cap`) with one entry per layer enabled in `cfg.layers`,
/// in draw order (§10.5.6), from `kOverlayColorsCvd` when `cvd` is true; returns the number of entries needed (may exceed `cap`;
/// call again with a larger buffer). `noexcept`; never allocates.
size_t ovt_render_legend(const ovt::render::OverlayConfig& cfg, bool cvd, ovt::render::LegendEntry* out, size_t cap) noexcept;
```

### 10.5.3 Grid generation algorithm

```
input: FrameAnalysis.blocks (tree), visible image rect V (expanded by 25 % each side), zoom, cfg.layers
for level in [CTU, CU, PU, TU] (codec mapping: H.264 MB→CTU slot, SubMB→PU slot, 4x4/8x8 transform→TU slot, no CU; AV1 SB→CTU, Block→CU, TXB→TU, no PU):
  if layer bit not set: continue
  min_px = smallest block side at this level (from blocks) * zoom
  if min_px < 6: continue                                      # R-GRID-1: never draw sub-6-px grids
  for block in blocks with kind == level and rect ∩ V ≠ ∅:     # iterate blocks[0].children recursively, prune by rect
    emit 4 lines (8 vertices) for block.rect edges (x, y, x+w, y+h), colour index per level
a shared edge belongs to two neighbouring blocks; to keep a doubly covered edge at the same alpha as a single one, **each edge is emitted exactly once**: every block emits only its **left and top** edges, and blocks touching the right/bottom picture boundary additionally emit those outer edges. (Rationale: libplacebo's `pl_blend_params` has only per-factor fields — `src_rgb/dst_rgb/src_alpha/dst_alpha` — and no blend-equation field, so a MAX equation (`GL_MAX`, available in GLES 3.0 core as `glBlendEquation(GL_MAX)`; `VK_BLEND_OP_MAX`) cannot be requested through `pl_pass`; emitting each edge once is portable and needs no scratch layer.)
```

Pixel snapping: in the vertex shader, line endpoints are transformed to device space and snapped to pixel centres (`floor(p) + 0.5`) so 1-px lines are crisp at every zoom; thickness is drawn as screen-aligned quads (2 triangles per line, so the "line list" is expanded to 6 vertices per line in the vertex shader via vertex pulling of the two endpoints; `K = 6`).

### 10.5.4 MV arrow pass

**List coding (single source for chapter 11 §11.6).** L0 and L1 arrows share one head geometry and differ by colour (§10.5.2) **and** by shaft pattern: L1 shafts are dashed 4 px on / 2 px off in device pixels (bit 1 of the instance `flags` selects the dash; implemented with the cumulative-length attribute of the line pass, §10.5.3), L0 and bi shafts are solid. This keeps the lists distinguishable without colour (FR-GUI-020 accessibility rule, chapter 11 §11.6 item 4).

Per instance `i` (texel `i`): origin `o` = centre of `MotionVector.rect` (luma samples), vector `v = (mv_x, mv_y) / 16 × cfg.mv_scale` (luma samples). The vertex shader computes device-space `O = view(o)`, `T = view(o + v)`, `L = |T − O|`. If `L < cfg.mv_min_px`: emit a 2×2 px dot at `O`. Else shaft = quad of width 1 px from `O` to `T − 4 px`, head = isoceles triangle with 6 px base and 6 px length at `T` (sizes × `device_pixel_ratio`). Colour by `list` and flags: list 0 → index 1, list 1 → index 2, both (bi) with `MvBi` → index 3, flags bit2 (global/warped) → index 21. Sub-block MVs (flags bit3: affine/OBMC) are drawn from their own `rect` centres (the plugin emits one `MotionVector` per sub-block). Field MVs (bit4) are drawn unchanged (vertical component in field units is a known inaccuracy; `Diagnostic RND-0060` once).

Visibility rule R-MV-1: arrows are drawn only when `cell_px = 4 × zoom ≥ 4` (a cell is ≥ 4 device px); above that bound the number of visible arrows is ≤ `viewport_px / 16` (518,400 at a 3840×2160 viewport). Instance textures hold all MVs of the frame (built once per frame); culling is done in the vertex shader by collapsing off-screen instances to degenerate triangles (zero cost beyond vertex processing).

### 10.5.5 Heatmap and label content

Heatmap value per cell `v ∈ [0, 1]` = `(x − min) / (max − min)` with the source column of `CellGrid`: `QP` (default range 0–51 for H.26x, 0–255 for AV1), `BitsX8` (`log2(1 + bits_x8/8)`, auto range), `MvMagnitude` (`sqrt(mv0_x² + mv0_y²)/16`, auto), `TxSize` (`tx_log2_w + tx_log2_h`, 4–12), `FilterIdx`, `RefIdx`, `IntraMode`, `MetricMap` (host-provided u16, e.g. per-block PSNR scaled to 0–65535 over the GUI's chosen range), `Custom`. The fragment shader: `color = texture(lut, v).rgb; alpha = cfg.heatmap_alpha` — cells with "not applicable" (−1 / 0xFF) get alpha 0.

Labels (`LabelContent`): one string per **block** at the current cycle level (not per cell), centred in the block, font size = `clamp(cell_px / 2, 9, 18) × dpr`; drawn only when `block_px_min ≥ cfg.label_zoom_threshold` (24). Strings: `BlockSize` "16x8"; `PredMode` "I"/"P"/"B"/"S"(kip)/"IBC"; `QP` "27"; `Bits` "312b" (bits_x8/8); `MvValues` "L0 −12,+3" in **quarter-sample units for H.264/HEVC and eighth-sample for AV1** (codec-native display; conversion from the 1/16 storage is a shift); `IntraMode` codec-numbered with name where known ("DC", "PLANAR", "ANG 26", "D45_PRED"); `TxType` "DCT2", "DST7", "ADST_FLIPADST"; `RefIdx` "L0[0]"; `BlockId` decimal.

### 10.5.6 Draw order (ascending; later passes over earlier)

1. Video (libplacebo `pl_render_image`, with overlays composited last by libplacebo — the overlay layer itself is drawn into in this order:)
2. Heatmap
3. Pred-mode / ref-idx fills
4. Skip hatch
5. Grid TU → PU → CU → CTU (fine first so coarse lines win)
6. Slice/tile boundaries
7. Deblock edges
8. Intra direction strokes
9. MV arrows: L1, then L0, then bi (cyan) so L0 reads on top of L1 by default
10. Hover outline, selection outline
11. Labels
12. Probe crosshair, selection rectangle

Id buffer writes happen in passes 3 (block fills write `block_id + 1`, enabled even when the fill layer is invisible — a separate "id-only" run of the fill pass with colour write masked) and 9 (arrows write `0x80000000 | mv_index`). Later passes do not write ids.

### 10.5.7 Picking algorithm

```
Pick VideoSurface::pick(px, py):
  (ix, iy) = inverse view transform (§10.2.3); hit = inside coded picture
  cell = (ix/4, iy/4); block_id = grid.block_id[cell]                      # CPU, O(1), always available when a FrameAnalysis is set
  id = idbuf_cpu[px, py]                                                   # from the last completed readback
  if id & 0x80000000: mv_index = id & 0x7FFFFFFF                           # arrows take precedence over blocks
  code[] = plane samples read from a 64×64-sample window around the cursor that the surface re-downloads with pl_tex_download
           whenever the cursor moves > 32 samples from the last window centre (≤ 3 × 8 KB per download; Picture memory is
           not retained by the surface, so textures are the source of truth)
  rgb_linear / nits / pq / xy: computed on the CPU from code[] with the same H.273 mapping (render/src/color/cpu_convert.cpp,
           shared with §10.7 tests; PQ EOTF per ST 2084 Eq. 4.1, HLG per BT.2100 Table 5 with Lw = 1000 and gamma 1.2)
```

Id-buffer readback: after each `render()`, the surface issues `pl_tex_download` of the full id buffer into a host-visible `pl_buf` (8.3 MB at 1080p; ~1 ms on UMA, asynchronous) and polls with `pl_buf_poll`; `pick()` uses the latest complete buffer (at most one frame stale — acceptable since the host re-picks on every mouse move). On GLES without `pl_buf` host mapping the surface downloads a 1×1 region at the last cursor position instead (`pick()` then reflects the previous frame's cursor; the GUI issues a redundant `pick()` after `render()`).

### 10.5.8 Performance plan (FR-BLK-018 / NFR-003; 4K reference: 3840×2160, 518,400 cells; 8K: 2,073,600 cells)

| Step | 4K cost (M1, measured target) | Technique |
|---|---|---|
| Plane upload (10-bit 4:2:0 as R16: 16.6 + 8.3 MB) | ≤ 2.0 ms | `pl_upload_plane` with persistent staging; skipped when `generation` unchanged |
| libplacebo render (4K → 1080p viewport, bilinear, no peak detect) | ≤ 4.0 ms | `pl_render_image`; shader cache `pl_cache` saved to `~/.cache/ovt/plcache.bin` |
| Cell attribute + heatmap textures (2 MB + 1 MB) | ≤ 0.5 ms | built on a worker thread, uploaded with `pl_tex_upload` |
| MV instance textures (16.6 MB) | ≤ 3.0 ms build (4 threads, SoA → texel), ≤ 1.0 ms upload | built on worker threads in `OverlayBuilder::build()`; cached per (decode_index, mv_scale) in a GPU LRU of 8 frames |
| Arrow pass (3.63 M vertices) | ≤ 3.0 ms | vertex pulling; off-screen instances collapsed |
| Grid lines (≤ 0.46 M vertices) | ≤ 0.5 ms | viewport culling R-GRID-1 |
| Labels (≤ 36 k glyphs) | ≤ 0.3 ms | SDF atlas |
| Id buffer + readback | ≤ 1.0 ms | async |
| **Total** | **≤ 15.3 ms** | measured by `bench_overlay_4k_mv_arrows` (chapter 14 §14.9.3), GPU timers via `pl_timer` |

8K rule R-8K-1: with 2.07 M cells the full-frame MV instance textures are 66 MB and 14.5 M vertices; the builder therefore switches to **viewport-culled instance building** when `cells > 1,048,576`: only MVs whose rect intersects the 25 %-expanded viewport are written (≤ 1.3 M at a 4K viewport with 4-px cells), rebuilt when the viewport leaves the margin. Grid, heatmap and labels are already bounded by the viewport. Android/ChromeOS budget (D5): p95 ≤ 33 ms (30 fps) at 1080p content on the Android reference device (Pixel Tablet, chapter 14 §14.9.2); 4K content on Android uses R-8K-1 culling at `cells > 262,144`.

---

## 10.6 Per-plane and diagnostic views

### 10.6.1 Plane modes (shader behaviour)

| `plane_mode` | Behaviour |
|---|---|
| 0 YUV | Normal libplacebo path (`pl_render_image`). |
| 1 Y | The surface builds a **second `pl_frame`** whose single plane is the Y texture with `repr.sys = PL_COLOR_SYSTEM_RGB`, `components = 1`, `component_mapping = {0, -1, -1}` (libplacebo's `component_mapping[]` maps each texture channel to at most one frame channel; a channel cannot be duplicated), and a one-line custom shader hook (`pl_hook` at `PL_HOOK_RGB_INPUT`: `color.rgb = color.rrr;`) that replicates Y to R,G,B, `repr.levels` = content range (so limited 64–940 expands to black–white) unless `raw_code_values`, in which case `repr.levels = PL_COLOR_LEVELS_FULL` and `transfer = SRGB` on both source and target so code values pass through unchanged to display values (`display = code >> (bit_depth − 8)` for 8-bit targets; 10-bit targets show the top 10 bits). `color.primaries = BT_709`. In HDR content, plane views are **always SDR views of code values** (never tone-mapped): they show what the encoder stored. |
| 2 U / 3 V | Same construction with the chroma texture as the single plane at **chroma resolution** (the frame reports the chroma plane size; crop scaled by `sub_x/sub_y`), upscaled with **nearest** to the viewport; value remapping in a tiny custom `pl_shader` (`pl_shader_custom`): `g = (c − 0.5) + 0.5` i.e. identity on normalised full-range chroma, so neutral chroma (code 512 of 10-bit) is mid-grey. Optional tint (`ViewState` flag, default off): U → blue/yellow axis (`rgb = (0.5 − d, 0.5 − d, 0.5 + d)`), V → red/cyan. |
| 4 gray | Normal path but chroma planes replaced by a 1×1 neutral texture (planes 1 and 2 of the `pl_frame` point to a constant mid-code texture: 128·2^(b−8) in the plane's bit depth) → correct luma rendering including tone mapping, no colour. |
| rotation / fields | `ViewState.rotation ∈ {0, 90, 180, 270}` (from `TrackInfo.rotation`, chapter 08, or user override) is applied as `pl_frame.rotation = PL_ROTATION_0/90/180/270` so the picture is upright; picking and the view transform operate in the rotated image space. `ViewState.field_mode ∈ {Frame, TopField, BottomField, Weave}` for interlaced pictures (`Picture.interlaced`, `top_field_first`): `TopField`/`BottomField` upload every second row of each plane (stride doubled, height halved) and display it at double vertical zoom; `Weave` is the default frame view; `Frame` shows the stored frame with a 1-px field-parity tint of the odd rows when `show_field_tint`. |

### 10.6.2 Magnification and sample-value text

- R-ZOOM-1: `zoom ≥ 1.0` → nearest-neighbour (`pl_filter_nearest`); `zoom < 1.0` → `ViewState.downscale` filter. Chroma upsampling to luma resolution for display follows `chroma_upscale`.
- R-ZOOM-2: zoom steps in the GUI are powers of two from 1/32 to 64 plus "fit"; the renderer accepts any positive double.
- R-ZOOM-3: sample-value text appears when `zoom ≥ 24` (device px per luma sample) and `show_sample_values`: in plane modes 1–3 the value of that plane's sample (decimal, or hex with `0x` when the host sets `OverlayConfig.hex_values`) centred in each sample; in mode 0 the Y value only; at `zoom ≥ 48` in mode 0 three lines "Y/U/V" (U/V from the co-sited chroma sample). Text colour auto-contrasts: white with black outline over values < 50 % grey, black with white outline otherwise.
- R-ZOOM-4: a 1-px sample grid (`#808080` A `0x66`) is drawn at `zoom ≥ 8`.

### 10.6.3 Pixel probe output

`Pick` (§10.2.6) feeds the GUI's probe read-out. Definitions: `code[]` are the stored code values; `rgb_linear` is linear light in the **content** primaries with 1.0 = 203 nits (libplacebo convention, i.e. `nits = value × 203`); `nits_luminance = Y` of XYZ computed with the content primaries' RGB→XYZ matrix (BT.2020: `Y = 0.2627 R + 0.6780 G + 0.0593 B`; BT.709: `0.2126, 0.7152, 0.0722`; P3-D65: `0.2290, 0.6917, 0.0793`) × 203; `nits_maxrgb = max(R,G,B) × 203`; `pq_code = PQ_OETF(nits_maxrgb / 10000)`; `cie_x = X/(X+Y+Z)`, `cie_y = Y/(X+Y+Z)`. For SDR content (`transfer` BT.1886/sRGB) the same formulas apply with the SDR assumption peak 203 nits.

### 10.6.4 Difference and compare views

All compare modes operate on **picture B resampled into A's coded grid** (identity when sizes match; `allow_scale_b` otherwise). Modes:

- `SideBySide`: viewport split in two; each half shows its picture with the same zoom/pan (locked), 2-px divider `#FFFFFF`.
- `Split`: one viewport; fragments left/above `split_position` take A, others B; divider line; the GUI drags it.
- `Overlay`: `out = (1 − α)·A + α·B` in display RGB after each is rendered to a linear FP16 intermediate (`pl_render_image` ×2 into offscreen textures, then a blend pass).
- `Flicker`: alternate A/B every `flicker_period_ms`; the surface requests redraws via the host's timer (it does not own a clock).
- `Difference`: computed in the **code-value domain per plane**, before any colour conversion, in a custom `pl_shader`: with `a, b ∈ [0,1]` = `code / (2^bd − 1)` (B shifted to A's bit depth), per plane `d_p = |a_p − b_p|`. Output grey `g = clamp(gain × d, 0, 1)` — written **directly as the output code value** (`out = round(g × 255)` for 8-bit targets; no transfer function is applied, so 1 code value of difference at gain `G` shows as `round(G × 255 / (2^bd − 1))` grey) — where `d = d_Y` for `diff_plane = 0`, `d_U/d_V` for 1/2 (at chroma resolution, nearest), `max(d_Y, d_U, d_V)` for −1 (chroma sampled nearest to luma). `diff_signed`: `g = clamp(0.5 + gain × (a − b) / 2, 0, 1)` (mid-grey = equal). `diff_false_color`: `rgb = turbo(g)`. The result is presented as a full-range BT.709 sRGB SDR frame (no tone mapping). The GUI shows the gain as "×N" and the implied threshold ("1 code value = N/1023 grey" etc.).
- Status read-out: a reduction pass over the visible region computes `MSE_Y` (and U, V) → `PSNR_p = 10·log10((2^bd − 1)² / MSE_p)` in code values; the result is `Measurements.region_psnr[3]`. This is a *display* convenience; authoritative metrics come from Chapter 9.

Mismatch states: B has different chroma format or bit depth → B is converted (chroma: nearest; depth: shift) with a yellow "converted" badge supplied via `last_error() = "RND-0011 B converted: 4:2:2→4:2:0"`. Different sizes without `allow_scale_b` → `RND-0010`, difference view shows a hatch.

---

## 10.7 HDR analysis measurements on GPU

### 10.7.1 Linear-light texture

For measurements the surface renders picture A once more (or, when `plane_mode == 0` and no compare mode, re-uses an intermediate) into a **measurement texture** `M`: `RGBA16F`, size = picture crop size downsampled by `s` (§10.7.3), `target.color = { primaries = source primaries, transfer = PL_COLOR_TRC_LINEAR, hdr.max_luma = 10000 }`, `tone_mapping_function = &pl_tone_map_clip`, no dithering, no peak detection, `upscaler/downscaler = pl_filter_box`. Because the target peak (10000) is ≥ any source peak, libplacebo performs **no tone mapping** and `M = linear light × (1/203)`: `nits = M × 203` (`PL_COLOR_SDR_WHITE`). HLG sources are linearised by libplacebo with the BT.2100 OOTF at `Lw = hdr.max_luma` of the source (1000 when absent). SDR sources are linearised with their EOTF and scaled so that reference white = 203 nits. Dolby Vision is measured after reshaping (what is displayed).

### 10.7.2 Per-pixel quantities (compute in a fragment pass `measure_prep.frag` into `Q`: `RGBA16F`)

```
rgb = texelFetch(M).rgb * 203.0            // nits, content primaries
maxrgb = max(rgb.r, max(rgb.g, rgb.b))     // CTA-861.3 MaxCLL basis
Y = dot(rgb, luma_coeffs)                  // CIE Y (nits), coefficients per primaries (§10.6.3)
pq = PQ_OETF(maxrgb / 10000.0)             // ST 2084 Eq. 5.1 (inverse EOTF): m1=0.1593017578125 m2=78.84375 c1=0.8359375 c2=18.8515625 c3=18.6875
XYZ = rgb_to_xyz(rgb); xy = XYZ.xy / sum(XYZ)
Q = vec4(maxrgb, Y, pq, out_of_gamut_flags)   // flags bit0: outside BT.709, bit1: outside P3 (§10.7.8)
```

### 10.7.3 Reductions and subsampling

- MaxCLL / min / FALL: a fragment-shader reduction chain with 8×8 → 1 steps over `Q.r` writing `RG32F (max, sum)` + `R32F (min)`: 4K → 480×270 → 60×34 → 8×5 → 1×1 (4 passes). The final texel is downloaded (`pl_tex_download`, 16 bytes). `fall_nits = sum / pixel_count`. **No subsampling** for these (exact over all pixels of the crop).
- Histogram (1024 PQ bins), waveform, vectorscope, CIE xy use **point accumulation**: a pass drawing one 1-px quad per sampled pixel (`K = 6` vertices, since `pl_pass` has no point primitive) reading `Q` via vertex pulling and emitting a point at the bin location with additive blending into an `R32F` target (`R16F` on GLES without `GL_EXT_color_buffer_float` → fall back to `RGBA8` with 8-bit counts and saturation, flagged `RND-0070`). Subsampling factor `s = max(1, ceil(sqrt(pixels / 2,073,600)))` (≤ 2.07 M samples: 4K → `s = 2`, 8K → `s = 4`, 1080p → 1); the sample grid is fixed (top-left of each `s×s` block) so results are deterministic. `sampled_pixels` reports the count.
- 99.99th percentile MaxCLL: from the histogram: smallest bin `i` such that cumulative count ≥ `0.9999 × sampled_pixels`; `max_cll_p9999_nits = PQ_EOTF((i + 1) / 1024) × 10000`. Resolution ≈ 0.1 % PQ.

### 10.7.4 Running (sequence) values

The GUI/CLI compute MaxCLL/MaxFALL over a range by stepping frames and calling `measure()`; the surface itself is per-frame. `ovt hdr --measure` (Chapter 12 §12.2.11; this chapter's WIs call it "hdr-scan") uses an offscreen surface at `s = 1`.

### 10.7.5 False colour (Table 10.7-1)

When `ViewState.false_color`, the displayed frame is replaced by `Q`-derived colours in a dedicated pass (`false_color.frag`) and presented as an SDR sRGB frame:

| Band | Range of `maxrgb` (nits) | Colour (sRGB) |
|---|---|---|
| 0 | < 1 | `#1F1F5C` (deep blue) |
| 1 | 1 – 100 | grey ramp: `#404040` at 1 nit → `#C0C0C0` at 100 nits (log interpolation) |
| 2 | 100 – 203 | `#2ECC40` (green; up to SDR reference white) |
| 3 | 203 – 1000 | `#FFDC00` (yellow) |
| 4 | 1000 – 4000 | `#FF851B` (orange) |
| 5 | > 4000 | `#FF4136` (red) |

Band boundaries are inclusive at the lower end. `Measurements.false_color_band_fraction[6]` is filled from the histogram using the PQ codes of the boundaries (1 nit ≈ 0.1499, 100 ≈ 0.5081, 203 ≈ 0.5807, 1000 ≈ 0.7518, 4000 ≈ 0.9026). The legend is drawn by the GUI (Chapter 11).

### 10.7.6 Luminance waveform

Target `W`: 1024 × 512 `R32F`. Column = `floor(sample_x / crop_w × 1024)`. Row (two axis modes, `ViewState`-adjacent `MeasureConfig.waveform_axis`): **log nits** (default): `row = 511 × (log10(clamp(maxrgb, 1e-4, 1e4)) + 4) / 8` (0.0001–10 000 nits over 8 decades); **PQ code**: `row = 511 × pq`. Three channel option: accumulate `Y`, or R, G, B separately into three targets (RGB parade). Display: the GUI draws `W` through a LUT (`log(1 + count)` → intensity, green or white) with horizontal reference lines at 1, 100, 203, 1000, 4000, 10000 nits labelled by the GUI.

### 10.7.7 Vectorscope and CIE xy

Vectorscope `V`: 512 × 512 `R32F`. Per sampled pixel compute **BT.2020 non-constant-luminance** `Cb, Cr` from the PQ-encoded signal: `R'G'B' = PQ_OETF(rgb_2020 / 10000)` (content RGB converted to BT.2020 primaries first via XYZ), `Y' = 0.2627R' + 0.6780G' + 0.0593B'`, `Cb = (B' − Y')/1.8814`, `Cr = (R' − Y')/1.4746`, then `u = 256 + Cb × 512 × gain`, `v = 256 − Cr × 512 × gain` (`gain` 1 or 2). Overlays drawn by the GUI: the BT.2020 100 % colour-bar hexagon (R, Y, G, C, B, M points), and the BT.709 and P3 primaries/secondaries converted to BT.2020 `CbCr` at 100 % saturation (precomputed table in `render/src/measure/gamut_tables.cpp`, 12 points each).

CIE xy `X`: 512 × 512 `R32F` covering `x ∈ [0, 0.8]`, `y ∈ [0, 0.9]`: `u = x / 0.8 × 512`, `v = (1 − y / 0.9) × 512`. The GUI overlays the spectral locus (CIE 1931 2° observer, 5-nm table 380–700 nm, 65 points, embedded `cie1931_locus.h`), the Planckian/D65 white point and the 709 / P3 / 2020 triangles.

### 10.7.8 Gamut-clip overlay and histogram image

Out-of-gamut flags (§10.7.2): convert linear content RGB to BT.709 RGB (3×3 via XYZ); `outside_709 = any(rgb709 < −0.001) || any(rgb709 > maxrgb_709_rel + 0.001)`: a pixel is outside when any component is negative or exceeds the luminance-relative maximum — simplified normative rule: `outside_709 = any(rgb709 < 0)`; same for P3. Display mode `gamut_clip_overlay`: pixels outside 709 are tinted magenta `#FF00FF` A `0x99` and outside P3 cyan `#00FFFF` A `0x99` over a greyscale picture (libplacebo's `pl_gamut_map_highlight` is *not* used here because its output depends on the target gamut; this pass is target-independent). Counts go to `Measurements`.

`histogram_image`: 1024 × 256 `RGBA8` rendered from `histogram_pq[]` (log scale bars) for hosts without plotting; the GUI may draw its own from the array.

### 10.7.9 Readback strategy

- All measurement passes run **after** the display render in the same `pl_gpu` submission; they are skipped entirely when no consumer asked (`measure()` not called within the last 2 renders; the HDR panel calls it every frame while visible).
- Downloads (`pl_tex_download` into `pl_buf` with `host_readable`) are double-buffered; `measure()` polls (`pl_buf_poll(buf, 0)`), copies completed data into the returned struct, marks `valid`, and never blocks. First call after a frame change returns the previous frame's data with `measured_frame` identifying it; the GUI shows the frame id next to the values.
- Scope textures are returned as `ExternalTexture` handles for the GUI to display through the same interop as the picture; for CLI use `export_png` dumps them.
- GPU timing via `pl_timer_create/pl_timer_query` fills `gpu_time_ms`.

---

## 10.8 Screenshots and export

```cpp
enum class ExportKind : uint8_t {
  HdrPng16,        // 16-bit RGB PNG, BT.2020 primaries, PQ transfer, full range, cICP chunk (9,16,0,1); no overlays; crop applied; native resolution
  SdrPng8,         // 8-bit sRGB PNG, tone-mapped with the supplied ViewState (tone_map, gamut_map), native resolution, no overlays
  ViewPng8,        // 8-bit sRGB PNG of the current viewport exactly as displayed (zoom, pan, overlays, compare), EMBEDDED-mode rendering
  ScopeWaveformPng, ScopeVectorscopePng, ScopeCiePng   // 8-bit PNG of the scope textures with the GUI-independent LUT
};
```

Rules: `HdrPng16` is produced by an offscreen `Rgb10A2`-equivalent render into an `RGBA16` UNORM texture with `target.color = {BT_2020, PQ}`, `repr.bits.color_depth = 16`, no dither, then written with **lodepng** (zlib licence, vendored `third_party/lodepng`) as colour type 2, bit depth 16, with a `cICP` chunk per the PNG Third Edition (W3C, https://www.w3.org/TR/png-3/#cICP-chunk) — values `colour_primaries = 9, transfer_function = 16, matrix_coefficients = 0, video_full_range_flag = 1`. For HLG sources `transfer_function = 18` and the data stays HLG-encoded (libplacebo target `HLG`). SDR sources exported as `HdrPng16` are written as 16-bit sRGB (`cICP 1,13,0,1`). `SdrPng8` uses the same path with an `RGBA8` sRGB target and `cICP 1,13,0,1`. File names default to `<stream>_<decode_index>_<display_index>_<kind>.png`.

Per-plane export (`PGM`, `Y4M`) is a **core** function (`ovt_picture_export_plane`, Chapter 12) operating on CPU planes: PGM `P5`, `maxval = 2^bd − 1`, 16-bit big-endian samples when `bd > 8`; Y4M with `C420jpeg/C420mpeg2/C420paldv/C422/C444` + `XYSCSS=` and `XCOLORRANGE=` tags. The renderer does not duplicate it; the GUI menu "Export plane…" calls core.

Overlay-only export (`ViewPng8` with `background_rgba = 0x00000000` and the picture hidden via a `ViewState` flag `hide_picture`) produces a transparent PNG of the overlays for documentation.

---

## 10.9 Test plan

### 10.9.1 Unit tests (`render/tests/`, Catch2, run in CI on all platforms with lavapipe/SwiftShader as the Vulkan device where no GPU exists)

| Test | Checks |
|---|---|
| `test_h273_map` | every row of §10.3.3 tables; MDCV order; unknown-code fallbacks and single logging |
| `test_pq_math` | `PQ_OETF`/`PQ_EOTF` round trip within 1e-6; reference points 1 nit → 0.1499, 100 → 0.5081, 203 → 0.5807, 1000 → 0.7518, 4000 → 0.9026, 10000 → 1.0 (tolerance 5e-4) |
| `test_plane_fill` | `pl_plane_data` for 8/10/12/16-bit × 4:0:0/4:2:0/4:2:2/4:4:4 × siting 0–3: strides, component sizes, `shift_x/y` values (left: −0.5, 0; center: 0, 0; top-left: −0.5, −0.5) |
| `test_overlay_builder` | synthetic `FrameAnalysis` (3 levels, 2 slices): vertex counts per level and viewport, culling at zoom thresholds, instance texel contents for 5 MVs, label strings |
| `test_pick` | id-buffer round trip on a synthetic frame: arrow at known device pixel → `mv_index`; cell → `block_id` |
| `test_hdr10plus_map` | parse the 3 ST 2094-40 payloads `tests/corpora/hdr10plus/{f000,f030,f059}.bin` (extracted from `syn_h265_hdr10plus`, chapter 14 §14.3.3 non-stream fixtures; reference JSON from `ffprobe` frame side data) |
| `test_android_dataspace` | compile-time constants vs NDK header |
| `fuzz_hdr10plus`, `fuzz_dovi_rpu_glue` | libFuzzer, 60 s smoke in CI |

### 10.9.2 Golden-image and parity tests (entries of the single manifest `tests/corpora/manifest.json` with tags `patterns` and `render-ref`, chapter 14 §14.3)

1. **Test patterns** generated by `tools/gen_hdr_patterns.py` (NumPy; writes Y4M + encodes with `x265 --lossless` / `aomenc --lossless` into `tests/corpora/patterns/`): `pq_patches_1000.mp4` (6 equal-area patches at 1/100/203/1000/4000/10000 nits — limited-range 10-bit codes 195/509/573/723/855/940, §10.4.4 — 10-bit limited BT.2020 PQ, 1920×1080, 3 frames; the file name records the 1000-nit *reference* patch, the pattern's MaxCLL is 10 000), `hlg_patches.mp4` (HLG 75 % and 100 % white, grey ramp), `sdr709_ramp.mp4`, `p3_saturation.mp4` (P3 primaries outside 709), `siting_checker.mp4` (4:2:0 2×2 checker to verify chroma siting). Corpus ids (chapter 14 §14.3.3 recipe 73, tag `patterns`; there is no separate pattern manifest): `syn_pat_pq_patches_1000`, `syn_pat_hlg_patches`, `syn_pat_sdr709_ramp`, `syn_pat_p3_saturation`, `syn_pat_siting_checker`; the file names above are their aliases (chapter 14 §14.3.5). The HLG demo stream is `syn_hlg_1080p` (recipe 38). Expected values are computed by the generator from ST 2084 and written next to the file (`*.expected.json`: nits per patch, FALL, band fractions, out-of-gamut counts).
2. **Measurement tests**: render each pattern offscreen, `measure()` → MaxCLL within 1 %, FALL within 1 %, band fractions within 0.5 % absolute, `out_of_gamut_709` for `p3_saturation` ≥ 99 % of the saturated area.
3. **libplacebo parity with mpv** (normative definition: Chapter 14 §14.4.15, suite WI-1.575): for the corpus frames listed there (8-bit 709, 10-bit PQ with MDCV, HLG, 4:4:4 12-bit, DV P8, HDR10+), `tests/oracles/mpv_capture.py` produces mpv `gpu-next` references with the fixed flag set of §14.4.15 (same libplacebo major as `third_party/VERSIONS.md`); OVT renders the same frame offscreen (`SdrPng8` with `ToneMap::BT2390`, `dither=false`, `downscale=Bilinear` at 1:1 for the SDR target; PQ pass-through 16-bit PNG for the HDR target). Pass criterion is `tools/imgdiff.py --mode hdr-parity`: **p99 ΔE_ITP ≤ 1.0 and mean ≤ 0.3 for the SDR target, p99 ΔE_ITP ≤ 0.5 for the HDR pass-through** (BT.2124 ΔE in linear light). PSNR gates are not used.
4. **Overlay goldens**: `synthetic_4k_grid.fb`, `synthetic_4k_mv.fb`, `labels_zoom32.fb` rendered to `ViewPng8` at fixed viewport 1920×1080, dpr 1.0 → compared with ≤ 0.1 % differing pixels (SDF text is deterministic; CI uses lavapipe references separate from GPU references, keyed by `pl_gpu` name).
5. **Determinism**: every golden render runs twice and must be byte-identical (FR-PLN-101, §10.1.1).

### 10.9.3 Performance tests

Render benchmarks live in `tests/perf/render/` on the shared Google Benchmark harness of chapter 14 §14.9 (`bench_render_video_4k`, `bench_overlay_4k_mv_arrows`, `bench_overlay_build`, `bench_render_first_frame`; run by `tools/perf_run.py` on the self-hosted M1 and Android runners of chapter 14 §14.9.2, budgets in `tests/perf/budgets.json`). Fixtures: `synthetic_4k_mv.fb` (518,400 MVs), `synthetic_8k_mv.fb`, `real_4k_hevc_frame.fb` and `real_1080p_hevc_frame.fb` (from `ovt analyze` of corpus streams; chapter 14 §14.3.3), 300 frames each with a scripted pan/zoom path; each benchmark reports p50/p95/p99 CPU and GPU times per step of §10.5.8. Inputs and budgets of all four benchmarks (M1 and `pixel-tablet` rows) are defined only in chapter 14 §14.9.3 and `tests/perf/budgets.json` (D19); this chapter repeats no numbers. The per-pass GPU times of §10.5.8 are reported as benchmark counters, not gated.

### 10.9.4 Manual HDR checklist (release gate, `docs/dev/hdr-checklist.md`)

MacBook Pro XDR: PQ pattern patches measured (i1Display or visual against `Display` panel readout); headroom change when adjusting brightness is tracked within 1 s. Linux Plasma 6.4 + Mesa 25.1 (AMD) and GNOME 48: Display panel reports max nits; PQ pattern 1000-nit patch brighter than UI white. HDR-capable Android device (`android-hdr`, chapter 14 §14.9.2) and HDR Chromebook: §10.4.4 criteria. Pixel Tablet (SDR panel, D12): HDR toggle disabled with reason `display has no HDR mode`, tone-mapped badge shown. Each line has a result cell in the release notes.

---

## 10.10 Diagnostic registry (`RND-*`)

Every `RND-*` code used in this chapter (and by chapter 11, which only displays them) is registered here (chapter 0 §0.7 and chapter 5 §5.7 diagnostic-code rule); the format is `RND-<nnnn>` (no qualifier). Codes marked *log* are emitted through `ovt::log` and `VideoSurface::last_error()` (they describe the renderer's state), codes marked *Diagnostic* are attached to the stream/picture as `Diagnostic` records and reach the Diagnostics panel. Codes are never reused or renumbered.

| Code | Severity | Message template | When raised | Spec reference |
|---|---|---|---|---|
| `RND-0001` | Error (log) | `picture rejected: bit_depth {bd} / sample_bytes {n} unsupported` | `VideoSurface::set_frame()` refuses a `Picture` with `bit_depth > 16` or `sample_bytes ∉ {1, 2}` (§10.2.1 rules); returns `false` | §10.2.1 |
| `RND-0002` | Error (log) | `renderer init failed: {stage}: {reason}` | `Renderer::init()` cannot create `pl_gpu`, the swapchain or the offscreen target (§10.2.7); `init()` returns `false` and the GUI shows the error state | §10.2.7 |
| `RND-0010` | Error (log) | `compare slot B is {wb}×{hb}, A is {wa}×{ha}; set allow_scale_b to resample` | compare mode with a B picture of different dimensions and `CompareMode.allow_scale_b == false`; the difference view shows a hatch | §10.2.4, §10.6.4 |
| `RND-0011` | Info (log) | `B converted: {from}→{to}` | compare slot B differs in chroma format or bit depth and is converted (chroma nearest, depth shift); the GUI shows the yellow "converted" badge | §10.6.4 |
| `RND-0020` | Warning (log) | `unknown H.273 code {n} for {field}, assuming {value}` | `h273_map.cpp` meets a `primaries`/`transfer`/`matrix` code it has no libplacebo mapping for; once per stream | §10.3.3; H.273 |
| `RND-0021` | Warning (Diagnostic) | `matrix coefficients {n} (chromaticity-derived) are unsupported; rendered approximately as BT.709` | `matrix ∈ {12, 13}` (chromaticity-derived NCL/CL) mapped to `PL_COLOR_SYSTEM_BT_709`; once per stream | §10.3.3 table; H.273 §8.3 |
| `RND-0030` | Warning (Diagnostic) | `DV profile 7 EL ignored` | Dolby Vision profile 7 dual-layer stream: the enhancement layer is not composed, the base layer is shown; once per stream | §10.3.7 |
| `RND-0031` | Info (log) | `DV reshaped` | a Dolby Vision profile 5/8 RPU was applied by libplacebo (`PL_COLOR_SYSTEM_DOLBYVISION`); drives the GUI's "DV" label; absent for pictures without `dovi_rpu` | §10.3.7 |
| `RND-0040` | Warning (log) | `no HDR surface format; falling back to Srgb8` | Wayland/Vulkan surface offers neither `{A2B10G10R10_UNORM_PACK32, HDR10_ST2084_EXT}` nor `{R16G16B16A16_SFLOAT, EXTENDED_SRGB_LINEAR_EXT}`; `init()` continues in SDR | §10.4.2 |
| `RND-0050` | Warning (log) | `ANativeWindow_setBuffersDataSpace failed ({code}); HDR output disabled for this session` | Android NATIVE path: the dataspace call returns non-zero; `hdr_capable = false`, EMBEDDED `Srgb8` is used | §10.4.3 |
| `RND-0060` | Info (Diagnostic) | `field motion vectors drawn in field units (vertical component approximate)` | first field MV (`MotionVector.flags` bit4) drawn by the arrow pass; once per stream | §10.5.4 |
| `RND-0070` | Info (log) | `float histogram target unavailable (no GL_EXT_color_buffer_float); using 8-bit counts with saturation` | GLES device without `GL_EXT_color_buffer_float`: waveform/histogram accumulate into `RGBA8` | §10.7.3 |

---

## 10.11 Work items

Conventions: all paths relative to the repository root; "render tests" = `ctest --test-dir build -R render_`; every WI adds its tests to `render/tests/CMakeLists.txt`. Dependencies on other chapters are named by their WI ranges (Chapter 7 decoders WI-1.1xx, Chapter 6 data model WI-0.0xx, Chapter 11 GUI WI-1.45x).

### Phase 1 — VideoSurface core, libplacebo, macOS EDR, Qt interop, plane views, compare modes

### WI-1.400 — Create `libovt-render` skeleton and libplacebo build integration
**Goal:** An empty `VideoSurface` that initialises a `pl_gpu` on an offscreen target on macOS and Linux.
**Depends on:** WI-0.001 (CMake presets), WI-0.015 (`third_party/libplacebo.cmake`), WI-0.017 (MoltenVK).
**Produces:** `render/CMakeLists.txt`, `render/include/ovt/render/{video_surface,picture,overlay_set,display_caps,export}.h` (all declarations of §10.2 as given), `render/src/surface/video_surface_impl.cpp`, `render/tests/test_init.cpp` (reduced per chapter 16 §16.3: `third_party/libplacebo.cmake` is WI-0.015's, MoltenVK acquisition is WI-0.017's).
**Interface:** §10.2.7 `VideoSurface::create/init/resize`; `DisplayTarget::Kind::OffscreenTexture`.
**Steps:** 1. Link `render/` against the libplacebo target of WI-0.015 and the Vulkan loader / MoltenVK of WI-0.017. 2. Write headers verbatim from §10.2. 3. Implement `init()` for `OffscreenTexture` creating `pl_log`, `pl_vk_inst_create`, `pl_vulkan_create`, an `RGBA8`/`RGBA16F` `pl_tex`. 4. `render()` clears to `background_rgba`. 5. `output_texture()` returns the handle; add a download helper for tests. 6. Test: init, render, download, all pixels = background.
**Acceptance tests:** 1. Given `OVT_BUILD_GUI=OFF`, When `cmake --preset linux-x64-debug && cmake --build`, Then `libovt-render.so` links and `render_test_init` passes under lavapipe. 2. Given macOS arm64, When the same, Then it passes with MoltenVK.
**Size:** M.
**Notes / pitfalls:** libplacebo's Meson build needs `glslang` for Vulkan shader compilation; vendor via vcpkg. Keep `pl_log` level at `PL_LOG_WARN` in tests, `PL_LOG_DEBUG` behind `OVT_RENDER_DEBUG=1`. **Overlap:** this WI delivers only the render skeleton and `render/CMakeLists.txt`; WI-0.015 owns `third_party/libplacebo.cmake` and WI-0.017 the MoltenVK acquisition. See chapter 16 §16.3.

### WI-1.401 — Implement `Picture` upload and `pl_frame` construction
**Goal:** `set_frame()` uploads 8/10/12/16-bit planar pictures and fills `pl_frame` per §10.3.1.
**Depends on:** WI-1.400, WI-1.413 (`syn_pat_siting_checker` pattern).
**Produces:** `render/src/pipeline/frame_upload.cpp`, `render/tests/test_plane_fill.cpp`.
**Interface:** §10.2.1 `Picture`, §10.3.1.
**Steps:** 1. Validate `Picture` (R-10.2.1 rules; `RND-0001`). 2. Per plane `pl_plane_data` + `pl_upload_plane` with persistent textures per slot. 3. `repr.bits` from bit depth; chroma siting table; crop. 4. `generation` skip logic. 5. Tests over the format matrix (synthetic ramps), checking `shift_x/y` and that a rendered 4:4:4 8-bit full-range RGB ramp (matrix 0) downloads identically.
**Acceptance tests:** 1. Given a 10-bit 4:2:0 limited ramp, When rendered 1:1 to `Rgba8Srgb`, Then luma code 64 → 0 and 940 → 255 (±1). 2. Given `tests/corpora/patterns/siting_checker.mp4` (`syn_pat_siting_checker`) frame 0, When rendered with siting 1 vs 2, Then chroma shifts by 0.5 sample (golden).
**Size:** M.
**Notes / pitfalls:** never pre-shift 10-bit samples; `pl_upload_plane` may pick a different texture format (e.g. R16 for 8-bit on some GPUs) — read `component_size` back, do not assume.

### WI-1.402 — H.273 → libplacebo colour mapping
**Goal:** Pure functions converting `ovt_ColorDescription` into `pl_color_repr` / `pl_color_space`.
**Depends on:** WI-1.400.
**Produces:** `render/src/color/h273_map.{h,cpp}`, `render/tests/test_h273_map.cpp`.
**Interface:** §10.3.3 tables.
**Steps:** 1. Implement the three tables as `switch` statements. 2. Range, MDCV (`mdcv_primaries` is R,G,B — no reordering here; the SEI/box G,B,R → R,G,B swap is done by the parsers, Chapter 7), CLL. 3. Unknown-code logging once per stream (keyed by the description hash). 4. Tests for every row and the guess fallbacks.
**Acceptance tests:** 1. Given each code in the tables, When mapped, Then the enum equals the table. 2. Given primaries 2 and 1920×1080, When mapped, Then `PL_COLOR_PRIM_BT_709`.
**Size:** S.
**Notes / pitfalls:** `mdcv_max_luminance` is in 0.0001 cd/m²; `max_cll` is already in cd/m².

### WI-1.403 — Target frames, tone/gamut mapping, dithering options
**Goal:** `render()` maps the source to any of the four encodings with the `ViewState` options.
**Depends on:** WI-1.401, WI-1.402.
**Produces:** `render/src/pipeline/render_params.cpp`, `render/tests/test_render_params.cpp`.
**Interface:** §10.3.2, §10.3.4, §10.3.5; `ViewState`.
**Steps:** 1. Build `pl_render_params` from `ViewState` (table of §10.3.4; R-TM-1). 2. Target frame per encoding (§10.3.2). 3. Dither rules. 4. Zoom/pan → `target.crop`; filters per §10.3.1. 5. Determinism test: render twice, compare.
**Acceptance tests:** 1. Given `pq_patches_1000.mp4` and `ToneMap::Clip` with `clip_nits = 500`, When rendered to `Rgba8Srgb`, Then the 1000-nit and 4000-nit patches have identical RGB. 2. Given any fixture, When rendered twice, Then byte-identical.
**Size:** M.
**Notes / pitfalls:** peak detection must be NULL except in `Auto`; `pl_render_params.dither_params` must be NULL for float targets or libplacebo asserts.

### WI-1.404 — Offscreen interop export (Metal texture, shared Vulkan device, GL texture)
**Goal:** The host can wrap the output texture without copies on each platform.
**Depends on:** WI-1.403.
**Produces:** `render/src/platform/offscreen.cpp`, `render/tests/test_interop_*.cpp` (platform-gated).
**Interface:** §10.2.2 interop table; `ExternalTexture`.
**Steps:** 1. `PL_HANDLE_MTL_TEX` export on macOS (`pl_tex_params.export_handle`), return `id<MTLTexture>`. 2. `pl_vulkan_import` from `VulkanDeviceInfo` on Linux; return `VkImage` + layout. 3. `pl_opengl_create` on a shared EGL context on Android; return `GLuint`. 4. Flush/sync rules.
**Acceptance tests:** 1. Given macOS, When a test creates a Metal device, wraps the exported texture and reads it back, Then pixels equal the libplacebo download. 2. Given Linux with a test `VkDevice`, When imported, Then `init()` succeeds and `output_texture().handle` is a valid `VkImage` readable by the test.
**Size:** L.
**Notes / pitfalls:** MoltenVK needs `VK_EXT_metal_objects` enabled at instance/device creation (libplacebo enables it when `export_handle` is requested — check `pl_gpu->export_caps.tex` includes the handle type before use).

### WI-1.405 — Qt `PictureHost` item using the offscreen texture (EMBEDDED mode)
**Goal:** The Picture panel shows the surface's texture via `QQuickRhiItem`.
**Depends on:** WI-1.404, WI-1.450 (GUI shell).
**Produces:** `app/src/picture_host.{h,cpp}` (`QQuickRhiItem` + `QQuickRhiItemRenderer`), `app/qml/panels/PicturePanel.qml` wiring.
**Interface:** `QRhiTexture::createFrom`, `ExternalTexture`.
**Steps:** 1. Force Qt RHI backend per platform (Metal/Vulkan/GLES) in `main.cpp`. 2. Renderer thread creates the `VideoSurface` with `OffscreenTexture` + interop. 3. On `synchronize()` copy `ViewState` from QML properties; on `render()` call `surface->render()` then draw the wrapped texture full-item. 4. Resize handling.
**Acceptance tests:** 1. Given the app opens `sdr709_ramp.mp4`, When shown, Then a screenshot of the item matches `SdrPng8` of frame 0 (≤ 0.1 % pixels differ). 2. Given a window resize, When the item size changes, Then `resize()` is called once per size and no flicker frame of background only is captured.
**Size:** M.
**Notes / pitfalls:** `QQuickRhiItem` renders into its own texture; wrapping an external texture means drawing a textured quad in its renderer — use `QRhiGraphicsPipeline` with a passthrough shader (`.qsb`), or copy via `QRhiResourceUpdateBatch::copyTexture`.

### WI-1.406 — macOS EDR native target (MoltenVK swapchain on CAMetalLayer)
**Goal:** NATIVE mode on macOS with `ScRgbLinear16F` and "system PQ" options.
**Depends on:** WI-1.403.
**Produces:** `render/src/platform/macos_metal.mm`, `render/tests/manual_edr_pattern.cpp` (manual); the app-side glue `app/src/platform/mac_layer.mm` (child `QWindow(MetalSurface)` → `CAMetalLayer*`) is delivered by WI-1.463 (chapter 11) against the `DisplayTarget` contract of this WI.
**Interface:** §10.4.1.
**Steps:** 1. Child `QWindow(MetalSurface)` in the app; obtain `CAMetalLayer`. 2. Layer properties per §10.4.1; `vkCreateMetalSurfaceEXT`; `pl_vulkan_create_swapchain` with the HDR format pair; fallback to `B8G8R8A8 + SRGB_NONLINEAR`. 3. `retarget()` between EMBEDDED and NATIVE. 4. Headroom polling and `retarget` on change. 5. Manual tool renders `pq_patches_1000`.
**Acceptance tests:** 1. Given an XDR display with HDR enabled, When NATIVE mode is entered, Then `active_encoding() == ScRgbLinear16F` and the 1000-nit patch is measured ≥ 4× the 203-nit patch luminance (colorimeter) or visibly brighter than UI white. 2. Given an SDR external display, When NATIVE requested, Then `init()` falls back to `Srgb8` and `DisplayCaps.hdr_capable == false`.
**Size:** L.
**Notes / pitfalls:** Set `layer.colorspace` *before* the first present; changing `wantsExtendedDynamicRangeContent` after presenting causes a visible flash. `winId()` forces native window creation — call it after the parent is exposed.

### WI-1.407 — Display capability probe (macOS first, with Linux/Android stubs)
**Goal:** `display_probe()` fills `DisplayCaps` on macOS; other platforms return "not implemented" reasons.
**Depends on:** WI-1.406.
**Produces:** `render/src/platform/display_caps_{macos.mm,linux.cpp,android.cpp}`, `render/tests/test_display_caps.cpp`.
**Interface:** §10.4.5.
**Steps:** 1. NSScreen queries; 2. probe surface format check; 3. `platform_detail` string; 4. re-probe triggers; 5. tests with a fake screen provider.
**Acceptance tests:** 1. Given an SDR-only Mac, When probed, Then `hdr_capable == false`, `reason == "display has no EDR headroom"`. 2. Given an XDR Mac, When probed, Then `potential_headroom ≥ 1.5`.
**Size:** S.
**Notes / pitfalls:** `maximumExtendedDynamicRangeColorComponentValue` is 1.0 until EDR content is on screen — use the *potential* value for `hdr_capable` and the current value for `headroom`; read both on the main thread.

### WI-1.408 — Plane modes Y/U/V/gray and raw code values
**Goal:** `plane_mode` 1–4 and `raw_code_values` behave per §10.6.1.
**Depends on:** WI-1.403.
**Produces:** `render/src/pipeline/plane_views.cpp`, goldens `plane_y_10bit.png`, `plane_u_nearest.png`, `plane_gray_pq.png`.
**Interface:** `ViewState.plane_mode`, `raw_code_values`.
**Steps:** 1. Secondary `pl_frame` builders for Y and chroma. 2. Chroma value remap custom shader. 3. Gray mode via neutral chroma texture. 4. Goldens.
**Acceptance tests:** 1. Given a 10-bit frame with luma code 512 and `raw_code_values`, When plane mode Y rendered to `Rgba8Srgb`, Then output grey = 128. 2. Given plane mode U at zoom 8 on 4:2:0, When rendered, Then each chroma sample covers 16×16 device px with sharp edges (golden).
**Size:** M.
**Notes / pitfalls:** `component_mapping` cannot duplicate a channel (§10.6.1) — replicate Y in the `PL_HOOK_RGB_INPUT` hook; keep the plane-view `pl_frame`s cached per slot so switching modes does not re-upload.

### WI-1.409 — Zoom, pan, nearest-neighbour magnification and sample-value text
**Goal:** Viewport mapping, zoom thresholds and value labels per §10.2.3 and §10.6.2.
**Depends on:** WI-1.408.
**Produces:** `render/src/pipeline/view_transform.cpp`, `render/tests/test_view_transform.cpp`.
**Interface:** §10.2.3 formulas; R-ZOOM-1..4.
**Steps:** 1. Transform + inverse; clamping helper for the host. 2. Filter selection by zoom. 3. Sample grid at zoom ≥ 8. 4. Expose the sample value under the cursor to the host (status bar / Selection Info).
**Acceptance tests:** 1. Given zoom 4, pan (100, 50), viewport 800×600, When mapping device (400, 300), Then image (100, 50); device (404, 300) → (101, 50). 2. Given zoom 8 on an 8-bit frame, When rendered, Then the 1-px sample grid is drawn and golden-matches `grid_zoom8.png`; given the cursor over sample (10, 20), When queried, Then the returned value equals the plane sample.
**Size:** S.
**Notes / pitfalls:** snap `target.crop` to whole device pixels at zoom ≥ 1 or nearest-neighbour sampling produces uneven sample widths; the inverse transform must use the same rounding as the forward one. Sample-value text at zoom ≥ 24 and its `labels_zoom32.png` golden belong to WI-2.433 (M2.1); in phase 1 the value under the cursor is shown in the status bar / Selection Info.

### WI-1.410 — Compare modes (SideBySide, Split, Overlay, Flicker, Difference)
**Goal:** `set_compare()` and all `CompareMode` kinds per §10.6.4.
**Depends on:** WI-1.403.
**Produces:** `render/src/pipeline/compare.cpp`, shaders `render/src/shaders/diff.glsl`, goldens `diff_gain8.png`, `split_v.png`.
**Interface:** §10.2.4.
**Steps:** 1. Slot B upload, resample policy, `RND-0010/0011`. 2. Side-by-side and split via two `pl_render_image` calls with scissor (`pl_frame.crop`/target crop). 3. Overlay blend pass. 4. Difference shader (code domain, gain, signed, false colour) + region PSNR reduction. 5. Flicker state.
**Acceptance tests:** 1. Given A and B identical, When Difference gain 32, Then all output pixels are 0 and `region_psnr[0] == +inf` (reported as 99.0). 2. Given B = A + 4 code values (10-bit) on luma, When gain 8, Then the output code value is `round(8 × 4/1023 × 255) = 8` (± 1; §10.6.4 writes `g` directly as the code value, no sRGB encoding).
**Size:** L.
**Notes / pitfalls:** compute the difference on the raw code-value textures before libplacebo's colour pipeline (a custom `pl_hook` or a separate `pl_pass`), never on the tone-mapped output; `region_psnr` must use the same visible-region rectangle as the viewport after clamping.

### WI-1.411 — HDR static metadata signalling and `pl_swapchain_colorspace_hint`
**Goal:** Native targets carry MDCV/CLL of the content.
**Depends on:** WI-1.406.
**Produces:** changes in `video_surface_impl.cpp`; `render/tests/test_hint.cpp` (verifies the hint struct built).
**Interface:** §10.3.6.
**Steps:** 1. Build the hint from the mapped colour space. 2. Call on init and on source change. 3. macOS: also update `CAEDRMetadata` in system-PQ mode.
**Acceptance tests:** 1. Given a PQ source with MDCV 1000/0.005, When hinted, Then the struct passed has `max_luma == 1000`, `min_luma == 0.005`.
**Size:** S.
**Notes / pitfalls:** `pl_swapchain_colorspace_hint` is advisory — re-read the swapchain's actual colour space after the next `pl_swapchain_start_frame` and log when it differs; on Android the EGL attributes are set by WI-A.401, not here.

### WI-1.412 — PNG export (`HdrPng16`, `SdrPng8`, `ViewPng8`)
**Goal:** `export_png()` per §10.8.
**Depends on:** WI-1.403, WI-1.410.
**Produces:** `render/src/export/png_export.cpp`, `third_party/lodepng/` (vendored, zlib licence recorded in `THIRD_PARTY_NOTICES.md`), `render/tests/test_export.cpp`.
**Interface:** `ExportKind`.
**Steps:** 1. Offscreen render per kind. 2. 16-bit PNG + `cICP` chunk via lodepng custom chunk API. 3. 8-bit PNGs. 4. Test: re-read PNG, verify `cICP` bytes and a patch value (1000-nit patch → PQ 16-bit code round(0.7518 × 65535) ± 32).
**Acceptance tests:** 1. Given `pq_patches_1000.mp4`, When `HdrPng16`, Then the PNG has `cICP = 09 10 00 01` and the 1000-nit patch value is within ±32/65535 of 0.7518. 2. Given the same, When `SdrPng8` with BT2390, Then `tools/imgdiff.py --mode hdr-parity` against the mpv reference of chapter 14 §14.4.15 passes (p99 ΔE_ITP ≤ 1.0, mean ≤ 0.3; §10.9.2 item 3).
**Size:** M.
**Notes / pitfalls:** lodepng writes `cICP` only through its custom-chunk API (`lodepng_chunk_create`) and the chunk must precede `PLTE`/`IDAT`; 16-bit samples are big-endian in PNG.

### WI-1.413 — Test pattern generator and measurement expectations
**Goal:** `tools/gen_hdr_patterns.py` and the `patterns` entries of the single corpora manifest for §10.9.2.
**Depends on:** none (Python 3.11, NumPy; x265/aomenc present in CI image).
**Produces:** `tools/gen_hdr_patterns.py`, `tests/corpora/patterns/*.{mp4,y4m,expected.json}`, the `syn_pat_*` entries (tag `patterns`, SHA-256) in `tests/corpora/manifest.json` (chapter 14 §14.3.3 recipe 73; no separate manifest).
**Interface:** output files consumed by WI-1.403, WI-1.412, WI-4.452.
**Steps:** 1. PQ OETF/EOTF in NumPy. 2. Patch patterns with exact limited/full codes. 3. HLG, SDR, P3 and siting patterns. 4. Expected JSON. 5. Lossless encodes.
**Acceptance tests:** 1. Given the script, When run twice, Then outputs are byte-identical. 2. Given `pq_patches_1000.expected.json`, When read, Then it lists nits per patch = [1, 100, 203, 1000, 4000, 10000], the limited-range codes [195, 509, 573, 723, 855, 940] and FALL = area-weighted mean. 3. Given `tests/corpora/manifest.json`, When `tools/check_fast_subset.py` runs, Then every generated pattern has a `syn_pat_*` entry with tag `patterns` and its SHA-256.
**Size:** S.
**Notes / pitfalls:** `x265 --lossless` keeps the exact codes only with `--input-depth 10 --output-depth 10` and no `--range` conversion; verify the decoded Y4M equals the generated one before committing the `.mp4`.

### WI-1.414 — libplacebo/mpv parity test harness (superseded by WI-1.575; see chapter 16 §16.3)
**Goal:** Automated comparison against mpv `gpu-next` screenshots.
**Overlap:** superseded by WI-1.575 (render parity suite with ΔE_ITP gates and `tests/oracles/mpv_capture.py`); the `render-ref` manifest entries are added by WI-0.552. Not implemented; its acceptance test moves to WI-1.575's PR checklist. See chapter 16 §16.3.
**Depends on:** WI-1.412, WI-1.413.
**Produces:** nothing (all files are produced by WI-1.575: `tests/oracles/mpv_capture.py`, `tests/golden/render/*.png`, `render/tests/test_render_parity.cpp`).
**Interface:** §10.9.2 item 3 = Chapter 14 §14.4.15.
**Steps:** none (superseded).
**Acceptance tests:** 1. Given the §14.4.15 reference frames, When `render_test_render_parity` runs, Then every frame passes `p99 ΔE_ITP ≤ 1.0` (SDR) / `≤ 0.5` (HDR pass-through).
**Size:** S (closed as superseded).
**Notes / pitfalls:** do not resurrect this WI; any change to the parity gate goes into WI-1.575 (chapter 14 §14.4.15 is the normative description).

### WI-1.415 — Render benchmarks (`bench_render_*`) and GPU timers
**Goal:** Performance harness of §10.9.3 (video path only; overlays added in WI-2.438).
**Overlap:** this WI registers the render benchmarks as Google Benchmark binaries in the harness of WI-0.560 (`tools/perf_run.py`, result JSON format, budgets in `tests/perf/budgets.json`); no separate threshold file. See chapter 16 §16.3.
**Depends on:** WI-1.403, WI-0.560.
**Produces:** `tests/perf/render/bench_render_video.cpp` (`bench_render_video_4k`, `bench_render_first_frame`), budget rows in `tests/perf/budgets.json`.
**Interface:** Google Benchmark flags of chapter 14 §14.9.1 plus `--fixture <fb> --frames N --viewport WxH`.
**Steps:** 1. Synthetic 4K picture generator. 2. Scripted zoom/pan path. 3. `pl_timer` per step; p50/p95/p99 in the result JSON. 4. Budget rows.
**Acceptance tests:** 1. Given the `m1-air-8gb` runner and the chapter 14 §14.9.3 input, When `bench_render_video_4k` runs, Then p95 meets its §14.9.3 budget. 2. Given the same runner, When `bench_render_first_frame` runs, Then `set_frame` → presented meets its §14.9.3 budget (warm and cold cache rows).
**Size:** S.
**Notes / pitfalls:** GPU timers (`pl_timer`) are unavailable on lavapipe — record CPU times only and mark the row `gpu_time: null`; never gate on a software device.

### Phase 2 — Overlays, picking, labels

### WI-2.430 — `OverlayBuilder` CPU side: cell attribute, heatmap and partition data
**Goal:** Build per-cell textures' CPU arrays from `FrameAnalysis` per §10.5.1/§10.5.5.
**Depends on:** WI-1.401, WI-0.083 (FlatBuffers schema, generated headers).
**Produces:** `render/src/overlay/builder.cpp`, `render/src/overlay/heatmap_source.cpp`, `render/tests/test_overlay_builder.cpp`, fixture generator `render/tests/fixtures/make_synthetic.cpp`.
**Interface:** §10.2.5.
**Steps:** 1. Verify the FlatBuffer (`Verifier`). 2. RGBA8UI cell attributes. 3. Heatmap normalisation per source incl. auto range and log2 bits. 4. Partition boundaries from `PartitionInfo` rects. 5. Multi-threaded (`ovt::jobs::Pool`, 4 chunks).
**Acceptance tests:** 1. Given the synthetic fixture, When built with `HeatmapSource::QP`, Then texel (cx, cy) equals `(qp − 0)/51 × 65535` for a known cell. 2. Given a cell with `pred == Skip`, When the attribute texture is built, Then attribute A bit0 is set.
**Size:** M.
**Notes / pitfalls:** verify the FlatBuffer once per `build()` and never trust `cells_w × cells_h` against the vector length without the verifier; sentinel values (−1 / 0xFF) must survive the texel packing.

### WI-2.431 — Overlay layer, LUTs and the heatmap/fill/hatch passes
**Goal:** Draw passes 2–4 of §10.5.6 into the overlay layer and composite via `pl_overlay`.
**Depends on:** WI-2.430, WI-1.403.
**Produces:** `render/src/overlay/overlay_layer.cpp`, `render/src/overlay/pass_fill.cpp`, `render/src/shaders/{fill,heatmap,hatch}.glsl`, `tools/gen_luts.py`, `render/luts/*.h`, golden `heatmap_qp_viridis.png`.
**Interface:** §10.5.1, §10.5.2, §10.5.5.
**Steps:** 1. LUT generator + 5 textures. 2. Overlay RGBA8 layer + `pl_overlay` wiring. 3. Fill pass (one full-viewport quad, `texelFetch` on cell attributes). 4. Hatch procedural. 5. Heatmap pass. 6. Goldens, including an HDR target check that overlay white equals 203 nits in `M`.
**Acceptance tests:** 1. Given the synthetic fixture with QP heatmap, When rendered, Then golden match. 2. Given a PQ target, When the overlay layer contains white, Then the measurement texture reads 1.0 (= 203 nits) at that pixel.
**Size:** M.
**Notes / pitfalls:** `pl_overlay` expects straight (non-premultiplied) alpha in `PL_OVERLAY_NORMAL` mode and composites in the target's colour space — set `overlay.repr`/`color` to sRGB BT.709 so white lands at reference white.

### WI-2.432 — Grid line pass with viewport/level culling
**Goal:** Partition grids per §10.5.3 with R-GRID-1.
**Depends on:** WI-2.431.
**Produces:** `render/src/overlay/pass_lines.cpp`, `render/src/shaders/lines.glsl`, golden `synthetic_4k_grid.png`.
**Interface:** `OverlayLayer::Grid*`, `GridPartition`.
**Steps:** 1. Block-tree traversal with rect pruning. 2. Left/top-edge emission rule. 3. Vertex pulling of endpoints → quads; pixel snapping. 4. Dashed partition lines. 5. Rebuild triggers (zoom threshold, pan margin). 6. Vertex-count assertions.
**Acceptance tests:** 1. Given the 4K fixture at fit zoom (0.5), When built, Then TU and PU levels emit 0 vertices and CTU emits 8160 × 8. 2. Given zoom 2 and a 1920×1080 viewport, When built, Then total vertices ≤ 480,000.
**Size:** M.
**Notes / pitfalls:** emit each edge once (left/top rule, §10.5.3); dashed lines need the cumulative length in a vertex attribute, not `gl_FragCoord`, or the pattern jumps at zoom changes.

### WI-2.433 — SDF glyph atlas and label pass
**Goal:** Labels and sample-value text per §10.5.5 and R-ZOOM-3.
**Depends on:** WI-2.431.
**Produces:** `tools/gen_sdf_atlas.py` (Pillow + NumPy distance transform), `render/fonts/dejavu_sans_mono_sdf.h`, `render/src/overlay/pass_labels.cpp`, `render/src/shaders/sdf_text.glsl`, golden `labels_zoom32.png`.
**Interface:** `LabelContent`, `label_zoom_threshold`.
**Steps:** 1. Atlas generation (checked in; licence file for DejaVu). 2. Glyph instance texture builder with string formatting per §10.5.5. 3. SDF fragment shader with outline. 4. Auto-contrast for sample values. 5. Sample-value text at zoom ≥ 24 (moved here from WI-1.409). 6. Goldens.
**Acceptance tests:** 1. Given `label_zoom_threshold = 24` and zoom 5 (cell = 20 px), When built, Then zero glyphs. 2. Given zoom 8 and `LabelContent::QP`, When rendered, Then one string per block and golden match. 3. Given zoom 32 on an 8-bit frame, When rendered, Then OCR-free golden match of `labels_zoom32.png` (sample-value text).
**Size:** M.
**Notes / pitfalls:** the SDF atlas is generated at build time from DejaVu Sans Mono (Bitstream Vera licence file committed next to it); SDF edge width must be scaled by `dpr` or text looks bold on HiDPI.

### WI-2.434 — MV arrow pass (vertex pulling) and intra direction strokes
**Goal:** Passes 8–9 of §10.5.6 per §10.5.4 at FR-BLK-018 scale (518 400 arrows at 4K).
**Depends on:** WI-2.431.
**Produces:** `render/src/overlay/pass_arrows.cpp`, `render/src/shaders/arrows.glsl`, fixture `synthetic_4k_mv.fb`, golden `synthetic_4k_mv.png`.
**Interface:** `OverlayLayer::MvL0/MvL1/MvBi/IntraDirs`, `mv_scale`, `mv_min_px`.
**Steps:** 1. Instance texture pair builder (threads). 2. Static `uint8` corner buffer (grown on demand). 3. Vertex shader (shaft+head, dots, culling collapse). 4. Colour rules incl. flags. 5. Intra strokes as 2-vertex variant. 6. R-MV-1 and R-8K-1 culled builder.
**Acceptance tests:** 1. Given 518,400 MVs, When rendered at zoom 1 on a 3840×2160 viewport on the M1 runner (test tagged `[.gpu]`, run only on the GPU runners of chapter 14 §14.9.2 and chapter 16 §16.5.3), Then the image golden-matches `synthetic_4k_mv.png` and the arrow-pass GPU time is reported as a benchmark counter (the frame budget is `bench_overlay_4k_mv_arrows`, chapter 14 §14.9.3). 2. Given an MV of (16, 0) (1 luma sample) at zoom 1, When rendered, Then it is drawn as a dot (`mv_min_px = 2`).
**Size:** L.
**Notes / pitfalls:** vertex pulling needs `textureSize`-independent indexing (`gl_VertexID / 9` → instance, `% 9` → corner); degenerate triangles for culled instances must have all three vertices identical or some tilers still rasterise a sliver.

### WI-2.435 — Id buffer and `pick()`
**Goal:** GPU id buffer, async readback and `Pick` per §10.5.7 and §10.6.3.
**Depends on:** WI-2.434.
**Produces:** `render/src/overlay/idbuffer.cpp`, `render/src/color/cpu_convert.cpp`, `render/tests/test_pick.cpp`.
**Interface:** `Pick`.
**Steps:** 1. R32_UINT layer written by fill and arrow passes. 2. Double-buffered download + poll. 3. CPU cell lookup. 4. 64×64 sample window download for code values. 5. CPU colour math (PQ/HLG EOTF, XYZ, xy) shared with tests. 6. GLES 1×1 fallback.
**Acceptance tests:** 1. Given the synthetic fixture, When `pick()` at the device pixel of a known arrow tip, Then `mv_index` matches. 2. Given a PQ frame with code 723 (10-bit limited) grey, When picked, Then `nits_maxrgb` = 1000 ± 1 %.
**Size:** M.
**Notes / pitfalls:** the id buffer must be cleared to `0xFFFFFFFF` every frame; readback buffers are reused only after `pl_buf_poll` reports completion.

### WI-2.436 — Deblocking-edge, ref-index fill and selection/probe passes
**Goal:** Passes 7, 3 (ref fill variant) and 10/12 of §10.5.6.
**Depends on:** WI-2.432.
**Produces:** `render/src/overlay/pass_misc.cpp`, goldens `deblock_edges.png`, `refidx_fill.png`, `selection.png`.
**Interface:** `OverlayLayer::DeblockEdges/RefIdxFill/Selection/Probe/SelectionRect`, `selected_block`, `selected_mv`, `probe_x/y`, `selection_rect`.
**Steps:** 1. Edge list from `deblock_bs_v/h` with the ≥ 8 px rule. 2. Ref fill via attribute G/B channels. 3. Selection outline (2 px white + 1 px black), hover, probe crosshair, rect. 4. Goldens.
**Acceptance tests:** 1. Given a selected block, When rendered, Then its outline pixels are `#FFFFFF` with a `#000000` 1-px halo (golden). 2. Given zoom 1 (cell 4 px), When rendered, Then no deblock edges are drawn.
**Size:** S.
**Notes / pitfalls:** the selection outline is drawn in device pixels (2 px + 1 px halo) regardless of zoom; ref-index fills use the LUT indices 22–29 of §10.5.2, not the heatmap LUT.

### WI-2.437 — GPU overlay cache and `set_overlays` lifecycle
**Goal:** LRU of 8 built `OverlaySet`s keyed by (decode_index, config hash) to make overlay toggling ≤ 1 frame.
**Depends on:** WI-2.434.
**Produces:** `render/src/overlay/overlay_cache.cpp`, `render/tests/test_overlay_cache.cpp`.
**Interface:** `set_overlays`, `OverlaySet` opaque.
**Steps:** 1. Hash `OverlayConfig` fields that affect GPU data (not selection/probe). 2. LRU with byte budget (256 MB desktop, 64 MB Android). 3. Eviction tests.
**Acceptance tests:** 1. Given 10 frames built in sequence, When frame 1 is requested again, Then it is rebuilt (evicted) and frame 10 is a hit.
**Size:** S.
**Notes / pitfalls:** selection, probe and `selection_rect` are *not* part of the cache key (they are uniforms); `mv_scale` is, because the instance textures bake it in.

### WI-2.438 — Overlay performance benchmarks (`bench_overlay_*`) and budget enforcement
**Goal:** Extend WI-1.415 with overlay fixtures and the §10.5.8 step table.
**Overlap:** the benchmarks are named `bench_overlay_4k_mv_arrows` and `bench_overlay_build` (§14.9.3) and gated by `tests/perf/budgets.json` of WI-0.560. See chapter 16 §16.3.
**Depends on:** WI-2.434, WI-2.433, WI-2.432, WI-1.415.
**Produces:** `tests/perf/render/bench_overlay.cpp` (`bench_overlay_4k_mv_arrows`, `bench_overlay_build`), budget rows in `tests/perf/budgets.json`, fixture `synthetic_8k_mv.fb`.
**Interface:** FR-BLK-018 / NFR-003 budgets as rows of `tests/perf/budgets.json` (values and inputs only in chapter 14 §14.9.3, D19: `bench_overlay_4k_mv_arrows`, `bench_overlay_build`).
**Steps:** 1. Load `.fb` fixtures. 2. Per-pass timers. 3. 8K culling path check. 4. Budget rows in `tests/perf/budgets.json` (no separate threshold file).
**Acceptance tests:** 1. Given the chapter 14 §14.9.3 input (`fa_4k_dense.bin`), When `bench_overlay_4k_mv_arrows` runs on the `m1-air-8gb` runner, Then p95 meets its §14.9.3 budget. 2. Given `synthetic_8k_mv.fb` at a 4K viewport, When the culled builder runs, Then instance texture bytes ≤ 48 MB (R-8K-1).
**Size:** S.
**Notes / pitfalls:** warm the shader cache before timing (`pl_cache` load) or the first frames dominate p95; report per-pass GPU times as separate benchmark counters.

### WI-2.439 — Colour-blind-safe overlay palette and legend data export
**Goal:** `kOverlayColorsCvd[]` and a C function the GUI uses to build legends.
**Depends on:** WI-2.431.
**Produces:** `render/include/ovt/render/overlay_colors.h` (`kOverlayColors`, `kOverlayColorsCvd`, `ovt_render_legend(OverlayConfig, out entries[])`), test.
**Interface:** §10.5.2.
**Steps:** 1. Tables. 2. Legend entries (name, RGBA, shape: fill/line/arrow/hatch) per enabled layer. 3. Contrast test (`render/tests/test_overlay_contrast.cpp`, criterion below). 4. CVD simulation check (deuteranopia/protanopia via the Machado 2009 matrices) that the CVD palette's L0/L1/bi/intra colours stay pairwise distinguishable (ΔE2000 ≥ 15 after simulation).
**Acceptance tests:** 1. Given `layers = MvL0|MvL1|GridCU`, When legend requested, Then 3 entries in draw order with the table colours. 2. **Contrast:** Given the 12 golden frames of §10.9.2 rendered to `SdrPng8` without overlays, When for each frame the 10 % darkest and 10 % lightest luma pixels are averaged, Then every line/arrow/label colour of `kOverlayColors[]` and `kOverlayColorsCvd[]` (indices 1, 2, 3, 8–21) composited at its table alpha has WCAG 2.x contrast ≥ 3:1 against **at least one** of the two averages, and every colour that fails against either average is drawn with its 1-px `#000000`/`#FFFFFF` halo (indices 15 and 19 already do; the test asserts the halo rule is set for the others in `OverlayConfig` defaults). 3. Given the CVD simulation of step 4, When applied to `kOverlayColorsCvd[]`, Then all pairwise ΔE2000 ≥ 15.
**Size:** S.
**Notes / pitfalls:** no single colour can hold 3:1 against both black and white video content — the halo rule is what guarantees legibility; the CVD palette is colour-blind-*safe* only for the four coded roles (L0, L1, bi, intra); heatmaps rely on `viridis`/`cividis`, and the frame-type palette on the shape coding of FR-GUI-020 (chapter 11 §11.4.5).

### Phase 4 — Linux Wayland HDR, HDR scopes, HDR10+/DV, exports

### WI-4.450 — Linux Vulkan native target over a child `QWindow(VulkanSurface)`
**Goal:** NATIVE mode on Wayland with `Pq10`/`ScRgbLinear16F` per §10.4.2.
**Depends on:** WI-1.406 (shared swapchain code), WI-1.450, WI-1.463 (NATIVE-mode host).
**Produces:** `render/src/platform/linux_vulkan_surface.cpp`, `app/src/platform/linux_child_window.cpp`.
**Interface:** `DisplayTarget::Kind::VulkanSurface`.
**Steps:** 1. App creates `QVulkanInstance` with the required extensions and the child window. 2. Surface enumerates formats, selects, falls back (`RND-0040`). 3. `retarget()`; resize via `pl_swapchain_resize`. 4. Manual test on Plasma ≥ 6.4 / GNOME ≥ 48 with Mesa ≥ 25.1.
**Acceptance tests:** 1. Given Plasma 6.4 + Mesa 25.1 (radv) with HDR enabled, When NATIVE requested, Then `active_encoding() == Pq10`. 2. Given an X11 session, When NATIVE is requested, Then `Srgb8` is used with `reason == "X11 session"`.
**Size:** L.
**Notes / pitfalls:** do not create a `wp_color_management_surface_v1` for the Vulkan window (Mesa owns it); NVIDIA proprietary lacks the WSI path — detect via `VkPhysicalDeviceProperties.driverID`.

### WI-4.451 — Wayland display-capability probe (`wp_color_manager_v1`)
**Goal:** `DisplayCaps` on Linux per §10.4.2/§10.4.5.
**Depends on:** WI-4.450, WI-1.407.
**Produces:** `render/src/platform/linux_wayland.cpp`, generated protocol code via `wayland-scanner` in CMake (`color-management-v1.xml` from wayland-protocols ≥ 1.41).
**Interface:** `display_probe`.
**Steps:** 1. Own registry on Qt's `wl_display`. 2. Bind manager + outputs; match by name. 3. Image description info events → fields. 4. Change events → re-probe callback.
**Acceptance tests:** 1. Given KWin with an HDR monitor, When probed, Then `max_nits > 203`, `source == "wp_color_manager_v1"`. 2. Given a compositor without the global, When probed, Then `reason == "compositor lacks wp_color_manager_v1"`.
**Size:** M.
**Notes / pitfalls:** bind the registry on a separate `wl_event_queue` so Qt's dispatch loop is not disturbed; `wl_output.name` events require version ≥ 4.

### WI-4.452 — Measurement texture, reductions and MaxCLL/FALL/histogram
**Goal:** §10.7.1–10.7.4 and 10.7.9.
**Depends on:** WI-1.403, WI-1.413.
**Produces:** `render/src/measure/{linear_pass,reduce,histogram}.cpp`, shaders `measure_prep.frag`, `reduce8.frag`, `accumulate.glsl`, `render/tests/test_measure.cpp`.
**Interface:** `measure()`, `Measurements`.
**Steps:** 1. Linear render into `M`. 2. `Q` pass. 3. Reduction chain. 4. Histogram accumulation with subsampling `s`. 5. Percentile. 6. Double-buffered readback, `valid`/`measured_frame` semantics. 7. GLES float-target fallback.
**Acceptance tests:** 1. Given `syn_pat_pq_patches_1000` frame 0 (patches up to 10 000 nits), When `measure()` runs, Then `max_cll_nits` = 10 000 ± 1 %, `max_cll_p9999` within the top band, FALL per expected JSON ± 1 %. 2. Given `syn_pat_hlg_patches` 75 % white, When `measure()` runs with a 1000-nit display model, Then the patch luminance is 203 ± 3 % (BT.2408 nominal for HLG at 1000-nit display, 75 % ≈ 203 nits).
**Size:** L.
**Notes / pitfalls:** MaxCLL is exact (no subsampling) — only the histogram-derived quantities are subsampled; HLG linearisation must use `Lw = 1000` when the stream carries no MDCV or the 75 % test fails.

### WI-4.453 — False colour and gamut-clip views
**Goal:** §10.7.5 and §10.7.8.
**Depends on:** WI-4.452.
**Produces:** `render/src/measure/false_color.cpp`, shaders, goldens `false_color_patches.png`, `gamut_clip_p3.png`.
**Interface:** `ViewState.false_color`, `gamut_clip_overlay`, band fractions, gamut counts.
**Steps:** 1. Band shader with log grey ramp. 2. Gamut flags in `Q`; count reduction. 3. Overlay tint pass. 4. Goldens.
**Acceptance tests:** 1. Given the PQ patches, When false colour, Then each patch is uniformly its band colour (golden). 2. Given `syn_pat_p3_saturation`, When `measure()` runs, Then `out_of_gamut_709 ≥ 0.99 × saturated_area_samples`.
**Size:** M.
**Notes / pitfalls:** band boundaries are evaluated on `maxrgb`, not on Y; the grey ramp of band 1 is log-interpolated.

### WI-4.454 — Waveform, vectorscope, CIE xy scope textures
**Goal:** §10.7.6–10.7.7 textures exposed via `ScopeTexture`.
**Depends on:** WI-4.452, WI-1.404.
**Produces:** `render/src/measure/scopes.cpp`, `render/src/measure/gamut_tables.cpp`, `render/src/measure/cie1931_locus.h`, tests `test_scopes.cpp`.
**Interface:** `Measurements.waveform/vectorscope/cie_xy`.
**Steps:** 1. Accumulation passes with the three coordinate mappings. 2. Axis modes. 3. Gamut point tables. 4. Export through `ExportKind::Scope*`. 5. Tests: a 203-nit flat frame → waveform energy only in the row for 203 nits (±1 row); a pure BT.2020 red frame → vectorscope energy at the red primary point (±2 px).
**Acceptance tests:** 1. Given a 203-nit flat frame, When the waveform is accumulated, Then all energy lies in the row for 203 nits (± 1 row). 2. Given a pure BT.2020 red frame, When the vectorscope is accumulated, Then the energy lies at the red primary point (± 2 px). 3. Given any fixture, When accumulated twice, Then the scope textures are byte-identical.
**Size:** M.
**Notes / pitfalls:** additive blending into `R32F` needs `GL_EXT_color_buffer_float` on GLES — fall back per §10.7.3 and flag `RND-0070`.

### WI-4.455 — HDR10+ (ST 2094-40) parsing and dynamic tone mapping
**Goal:** §10.3.6 dynamic metadata path.
**Depends on:** WI-1.403, WI-1.114 (H.265 SEI incl. the ST 2094-40 payload), WI-1.116 (AV1 metadata OBU), WI-1.223 (`HdrMetadata` view).
**Produces:** `render/src/color/hdr10plus_map.cpp`, `render/tests/test_hdr10plus_map.cpp`, `render/tests/fuzz_hdr10plus.cpp`.
**Interface:** `Picture.hdr10plus_payload`.
**Steps:** 1. `BitReader` parser of the application-4 ST 2094-40 syntax (windows, maxscl, average_maxrgb, Bezier). 2. Map to `pl_hdr_metadata`. 3. Compare against FFmpeg's `AVDynamicHDRPlus` dump for 3 samples. 4. Fuzz target.
**Acceptance tests:** 1. Given sample payloads, When parsed, Then fields equal FFmpeg's. 2. Given `fuzz_hdr10plus` with random input, When it runs for 60 s, Then no crash, leak or timeout is reported.
**Size:** M.
**Notes / pitfalls:** ST 2094-40 Bezier anchors are 10-bit fixed point (÷ 1023); `targeted_system_display_maximum_luminance` is in units of 1/32 cd/m².

### WI-4.456 — Dolby Vision RPU reshaping (P5/P8) and UI labelling hook
**Goal:** §10.3.7 policy.
**Depends on:** WI-1.403, WI-2.172 (`ovt_ColorDescription` filled from `AVDOVIMetadata`).
**Produces:** `render/src/color/dovi_map.cpp`, parity reference for a DV P8.1 sample vs mpv.
**Interface:** `Picture.dovi_rpu`, `ovt_ColorDescription.has_dolby_vision/dv_profile`; `last_error()` info string `RND-0031 "DV reshaped"` for the GUI label.
**Steps:** 1. Map via `pl_map_dovi_metadata`. 2. `PL_COLOR_SYSTEM_DOLBYVISION`. 3. Profile 7 EL diagnostic. 4. Parity with mpv for the P8 sample per Chapter 14 §14.4.15 (p99 ΔE_ITP ≤ 1.0).
**Acceptance tests:** 1. Given the DV P8 corpus sample, When rendered to `SdrPng8` with BT2390, Then parity passes. 2. Given a DV profile 7 stream, When rendered, Then `RND-0030` is raised once.
**Size:** M.
**Notes / pitfalls:** the input is the `AVDOVIMetadata` side-data blob, not an RPU NAL (§10.3.7); check `av_dovi_get_header(...)->rpu_type == 2` before mapping.

### WI-4.457 — Scope and HDR exports in the CLI (`ovt render`; `ovt hdr --measure` is WI-3.532)
**Goal:** Headless use of the surface for sequence MaxCLL/MaxFALL and PNG dumps.
**Overlap:** this WI delivers only `ovt render`; the earlier HDR-scan subcommand is folded into `ovt hdr --measure --frames a-b` of WI-3.532 and `hdr_scan.schema.json` is not written. See chapter 16 §16.3.
**Depends on:** WI-4.452, WI-1.412, WI-1.500 (CLI skeleton), WI-3.532 (`ovt hdr` owns sequence measurement; this WI delivers `ovt render` only).
**Produces:** `cli/src/cmd/render.cpp` (`ovt render`, Chapter 12 §12.2.18) and the headless `render/src/export/headless_surface.cpp` helper that WI-3.532's `cli/src/cmd/hdr.cpp` (`ovt hdr --measure`) also links; the JSON shape is `HdrReport` (Chapter 6 §6.3), no separate schema file.
**Interface:** `ovt hdr <file> --measure [--track N] [--frames a-b] -f json -o out.json` → `HdrReport.measures[]` per frame (`max_nits`, `fall_nits`, `p99_nits`, …) + `max_cll_measured`/`max_fall_measured` (the subcommand name `hdr-scan` is **not** accepted; Chapter 12 §12.2.11 is normative); `ovt render <file> --frame N --kind hdr16|sdr8|view --out f.png` (Chapter 12 §12.2.18).
**Steps:** 1. Offscreen surface at `s = 1`. 2. Frame iteration via the playback decoder. 3. JSON writer. 4. Tests on patterns.
**Acceptance tests:** 1. Given `pq_patches_1000.mp4`, When `ovt render --frame 0 --kind hdr16 --out f.png` runs, Then the PNG equals the `HdrPng16` export of WI-1.412 byte for byte. 2. Given the same file, When `ovt hdr --measure --format json` (WI-3.532, using this WI's headless surface) runs, Then `max_cll_measured == 10000 ± 1 %`.
**Size:** M.
**Notes / pitfalls:** the headless surface must run with `s = 1` and peak detection off (R-TM-1); `ovt render` never shares a `pl_gpu` with a GUI.

### WI-4.458 — Android display-capability probe and headroom listener
**Goal:** `DisplayCaps` on Android per §10.4.3/§10.4.5 (native side; Kotlin side in WI-A.404).
**Overlap:** track-A phase-1 deliverable (milestone MA.1) because WI-A.404 depends on it. See chapter 16 §16.3.
**Depends on:** WI-A.401, WI-1.407.
**Produces:** `render/src/platform/display_caps_android.cpp` (JNI entry `ovt_render_set_display_caps` called by Kotlin).
**Interface:** `DisplayCaps` (§10.4.5); JNI signature `nativeSetDisplayCaps(int[] hdrTypes, float maxNits, float minNits, float maxFall, float hdrSdrRatio, int apiLevel)`.
**Steps:** 1. JNI struct marshalling. 2. Re-probe hook → `retarget()`.
**Acceptance tests:** 1. Given the Pixel Tablet (SDR panel, D12), When probed, Then `hdr_capable == false`, `supports_pq == supports_hlg == false`, `reason == "display has no HDR mode"` and `source == "Display.Mode.supportedHdrTypes"`. 2. Given the HDR-capable Android device `android-hdr` of chapter 14 §14.9.2, When probed, Then `supports_pq == true` and `max_nits > 203`.
**Size:** S.
**Notes / pitfalls:** `getSupportedHdrTypes()` on the *mode* (API 34) may list types the current mode cannot show — re-probe on `onDisplayChanged`.

### WI-4.459 — Manual HDR release checklist and documentation
**Goal:** `docs/dev/hdr-checklist.md` and `docs/dev/display-capabilities.md` describing §10.4 behaviour per platform for support staff.
**Depends on:** WI-4.450, WI-1.406, WI-0.055 (ChromeOS HDR spike result).
**Produces:** the two documents; release-notes template section.
**Interface:** §10.9.4 checklist rows; chapter 14 §14.10.5.
**Steps:** 1. Write the per-platform procedure (macOS XDR, Plasma/GNOME, HDR Android device, Chromebook, Pixel Tablet SDR). 2. Result template. 3. Link from the release-notes template.
**Acceptance tests:** 1. Given a release candidate, When the checklist is executed, Then every row has a recorded result.
**Size:** S.
**Notes / pitfalls:** the checklist is the only place where absolute-luminance claims are verified with a colorimeter; CI cannot replace it.

### Phase A — Android

### WI-A.400 — HDR `SurfaceView` sibling in the Android shell
**Goal:** `HdrSurfaceView.kt`: a `SurfaceView` positioned over the Picture panel, lifecycle-safe.
**Overlap:** this WI extends `HdrSurfaceView.kt` and `hdr_surface.cpp` of WI-0.052 with the geometry callbacks, visibility rule and lifecycle tests; no `HdrSurface.kt`/`hdr_surface_jni.cpp`. See chapter 16 §16.3.
**Depends on:** WI-0.050 (Android Gradle project with `QtQuickView`), WI-0.052 (`HdrSurfaceView.kt` skeleton), WI-A.450.
**Produces:** extensions to `android/app/src/main/kotlin/org/ovt/HdrSurfaceView.kt` and `android/app/src/main/cpp/hdr_surface.cpp` (geometry callbacks, visibility rule), `android/app/src/androidTest/java/org/ovt/HdrSurfaceGeometryTest.kt`.
**Interface:** QML → Kotlin geometry callback `onPictureRectChanged(x, y, w, h)` (window px); JNI `nativeSurfaceCreated(Surface)`, `nativeSurfaceChanged(w, h)`, `nativeSurfaceDestroyed()`, `nativeSetVisible(bool)`.
**Steps:** 1. `FrameLayout` with `QtQuickView` + `SurfaceView(setZOrderMediaOverlay(true))`. 2. Geometry updates on the UI thread. 3. `window.colorMode = COLOR_MODE_HDR`. 4. Visibility toggling (popup rule R-ARCH-3).
**Acceptance tests:** 1. Given the Picture panel is resized by dragging a splitter, When measured, Then the `SurfaceView` bounds equal the panel bounds within 1 px on every frame. 2. Given the Activity is rotated, When the configuration change completes, Then the surface is recreated and `init()` succeeds without leaking the previous `ANativeWindow` (LeakCanary clean).
**Size:** M.
**Notes / pitfalls:** `ANativeWindow_fromSurface` adds a reference — release it in `nativeSurfaceDestroyed`; `setZOrderMediaOverlay` must be called before the view is attached.

### WI-A.401 — EGL + dataspace native target (`AndroidWindow`)
**Goal:** `render/src/platform/android_egl.cpp` per §10.4.3 with libplacebo's OpenGL backend.
**Depends on:** WI-1.403, WI-A.400.
**Produces:** `render/src/platform/android_egl.cpp`, `render/tests/test_android_dataspace.cpp`, `android/app/src/androidTest/java/org/ovt/HdrSurfaceTest.kt`.
**Interface:** `DisplayTarget::Kind::AndroidWindow`; encoding selection rule.
**Steps:** 1. Dataspace before EGL surface. 2. RGB10A2 config; `EGL_EXT_gl_colorspace_bt2020_*` when present. 3. SMPTE2086/CTA861.3 surface attribs. 4. `pl_opengl_create` with make/release-current callbacks; swapchain. 5. SDR fallback.
**Acceptance tests:** 1. Given the HDR-capable Android device `android-hdr` of chapter 14 §14.9.2 (HDR10 and/or HLG in `supportedHdrTypes`), When NATIVE is requested for a PQ stream, Then `active_encoding() == Pq10` (or `Hlg10` on an HLG-only panel), `ANativeWindow_setBuffersDataSpace` returned 0 and `adb logcat -d -s OVT_HDR` shows `dataspace=BT2020_PQ` (line format of chapter 13 WI-0.052). 2. Given the Pixel Tablet (SDR panel) or an SDR-only emulator, When a PQ stream is opened, Then NATIVE is never requested (`hdr_capable == false`), EMBEDDED `Srgb8` is used and no `RND-0050` is logged. 3. Given an HDR-capable device whose `ANativeWindow_setBuffersDataSpace` returns non-zero (fault injection in the test), When NATIVE is requested, Then `Srgb8` is used, `RND-0050` is logged once and `OVT_HDR dataspace=SRGB` is logged.
**Size:** L.
**Notes / pitfalls:** `eglCreateWindowSurface` with a 10-bit config can fail on some Mali drivers — retry with `EGL_RED_SIZE 8` and log; GLES must be 3.1+ for libplacebo compute; check `GL_VERSION`.

### WI-A.402 — ChromeOS HDR hardware spike
**Goal:** Execute §10.4.4 and record the verdict.
**Overlap:** superseded by WI-0.055 (ChromeOS/Android HDR hardware spike, phase 0 week one); the §10.4.4 pass/fail criteria become the Interface of WI-0.055 and the verdict is recorded in `docs/platforms/chromeos-hdr.md`. See chapter 16 §16.3.
**Depends on:** none (standalone app).
**Produces:** `android/spike-hdr/` (Kotlin + NDK), `docs/dev/chromeos-hdr-spike.md`.
**Interface:** the pass/fail criteria of §10.4.4.
**Steps:** 1. Spike app. 2. Run on an HDR Chromebook, on the `android-hdr` device (positive control) and on the Pixel Tablet (SDR negative control). 3. Record logs/measurements. 4. Decision recorded; owner informed (risk register, Chapter 17).
**Acceptance tests:** 1. Given the document, When reviewed, Then it contains device, OS versions, dataspace rc, `supportedHdrTypes`, measurements and a PASS/FAIL line.
**Size:** M.
**Notes / pitfalls:** on ChromeOS `dumpsys SurfaceFlinger` runs inside ARCVM — use `adb` over the Crostini bridge; the Exo pass-through verdict can differ between ChromeOS channels, record the exact build.

### WI-A.403 — Shared-context GL interop for EMBEDDED mode on Android
**Goal:** `Interop::GlTexture` with a context shared with Qt's.
**Depends on:** WI-1.404, WI-A.450.
**Produces:** `android/app/src/main/cpp/gl_share.cpp`, `app/src/picture_host_android.cpp`.
**Interface:** `Interop::GlTexture` (§10.2.2 Android row), fence rule of that row.
**Steps:** 1. Obtain Qt's `EGLContext` via `QOpenGLContext::nativeInterface<QNativeInterface::QEGLContext>()`. 2. Create a shared context on the render thread. 3. `pl_opengl_create` with callbacks. 4. `glFenceSync`/`glWaitSync` in the share group (GLES 3.0 core), `EGL_KHR_fence_sync` across groups, `glFinish` only behind `OVT_RENDER_GL_FINISH=1`.
**Acceptance tests:** 1. Given the emulator, When a frame is shown in EMBEDDED mode, Then the Qt item screenshot equals `SdrPng8` ≤ 0.1 % differing. 2. Given 300 frames with the fence path, When presented, Then no frame shows a partially rendered texture (checksum of each presented frame equals an offscreen render).
**Size:** M.
**Notes / pitfalls:** Qt's GLES context on Android is created on the render thread — create the shared context from that thread's `eglGetCurrentContext()` or `EGL_BAD_MATCH` follows.

### WI-A.404 — Kotlin display-capability and headroom plumbing
**Goal:** display HDR capability query (`Display.getMode().getSupportedHdrTypes()` on API ≥ 34, `Display.getHdrCapabilities()` fallback below), `hdrSdrRatio` listener, `setDesiredHdrHeadroom` per §10.4.3.
**Overlap:** this WI extends `DisplayCaps.kt` created as a stub by WI-0.052. See chapter 16 §16.3.
**Depends on:** WI-A.400, WI-4.458.
**Produces:** `android/app/src/main/kotlin/org/ovt/DisplayCaps.kt`.
**Interface:** `nativeSetDisplayCaps(...)` of WI-4.458; `Display.Mode.getSupportedHdrTypes()` (API 34+), `Display.getHdrCapabilities()` (API < 34 types; luminance on all levels), `Display.getHdrSdrRatio()` (API 34), `SurfaceView.setDesiredHdrHeadroom()` (API 35).
**Steps:** 1. Query and marshal to JNI. 2. API-level guards (28/34/35). 3. Headroom 2.0 mixed / 8.0 full-screen.
**Acceptance tests:** 1. Given API 35, When the Picture panel enters full-screen, Then `setDesiredHdrHeadroom(8.0f)` is called (verified by instrumentation test). 2. Given the Pixel Tablet, When the app queries the display, Then `hdrTypes` is empty and the HDR toggle stays disabled.
**Size:** S.
**Notes / pitfalls:** `registerHdrSdrRatioChangedListener` throws when `isHdrSdrRatioAvailable()` is false — guard it; unregister in `onStop`.

### WI-A.405 — Android performance tuning for overlays (GLES)
**Goal:** Meet the D5 Android overlay budget (chapter 14 §14.9.3 `pixel-tablet` row) on the Pixel Tablet with all overlays.
**Depends on:** WI-2.438, WI-A.403 (GLES interop; HDR output is not needed for performance).
**Produces:** Android budget rows in `tests/perf/budgets.json` (`bench_overlay_4k_mv_arrows` on the `pixel-tablet` runner at 1080p), culling threshold tuning (R-8K-1 at 262,144 cells), `android/app/src/androidTest/java/org/ovt/RenderBenchTest.kt` (drives the benchmark binary).
**Interface:** chapter 14 §14.9.3 budget table; `tools/perf_run.py --runner pixel-tablet`.
**Steps:** 1. Run the render benchmarks via the instrumentation harness. 2. Profile with AGI/Perfetto. 3. Tune texture formats (RGBA16F vs RGBA32F instance data where precision allows).
**Acceptance tests:** 1. Given the chapter 14 §14.9.3 Android input (`fa_1080p_dense.bin`) and, as a second run, `real_1080p_hevc_frame.fb`, When `bench_overlay_4k_mv_arrows` runs on `pixel-tablet`, Then p95 meets the §14.9.3 `pixel-tablet` budget for both inputs.
**Size:** M.
**Notes / pitfalls:** Mali/Adreno tilers penalise mid-frame readbacks — move the id-buffer download after `eglSwapBuffers`; `RGBA16F` instance data loses precision above 2048-sample MVs, keep RGBA32F for those.

### WI-A.406 — `AHardwareBuffer` + Vulkan path (optional, gated)
**Goal:** Alternative presentation per §10.4.3 if WI-A.405 fails by > 2×.
**Depends on:** WI-A.405 result.
**Produces:** `render/src/platform/android_ahb.cpp`.
**Interface:** `DisplayTarget::Kind::AndroidWindow` with `native2 = ASurfaceControl*`; §10.4.3 "`AHardwareBuffer` alternative".
**Steps:** 1. `ASurfaceControl` from the `SurfaceView`. 2. Vulkan images bound to `AHardwareBuffer`. 3. Transactions with dataspace + metadata. 4. libplacebo renders to wrapped images (`pl_vulkan_wrap`).
**Acceptance tests:** 1. Given the `android-hdr` device, When enabled, Then the PQ pattern displays identically to the EGL path (§10.4.4 criteria) and p95 improves ≥ 1.5×.
**Size:** L.
**Notes / pitfalls:** `ASurfaceTransaction_setHdrMetadata_*` expects CTA-861.3 values in nits and ST 2086 primaries as floats (no ×50000 scaling, unlike EGL).

---

### Glossary additions (chapter 10)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **`color-management-v1`** — Wayland protocol (wayland-protocols ≥ 1.41) by which clients describe their surfaces' colour and query output capabilities.
- **Waveform / vectorscope / CIE xy plot** — scopes showing luminance vs horizontal position, chroma distribution (Cb/Cr), and chromaticity distribution respectively.
