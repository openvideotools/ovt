# HDR output on Android

How OVT Studio shows HDR video on Android and ChromeOS, and how to check it on a device (WI-0.052; chapter 13
§13.4.4 item 2, chapter 10 §10.4.3, ADR-014). The hardware verdicts of the HDR spike (WI-0.055) are recorded in
the "Device results" section below and, for Chromebooks, in `docs/platforms/chromeos-hdr.md`.

## How it works

`MainActivity` hosts two siblings in one `FrameLayout`:

- `HdrSurfaceView`, a `SurfaceView` (never a `TextureView`, which forces SDR on API 33+), in the default z-order:
  below the window, so below the Qt UI.
- `QtQuickView` on top. The Qt window is translucent (`QSurfaceFormat` alpha 8, `QQuickWindow` colour transparent),
  and `AndroidShell.qml` paints only its header band, so the surface shows through below it. Until WI-A.400 follows
  the Picture panel, the surface fills the window below the top quarter.

Start-up order:

1. `DisplayCaps.query()` reads the display: HDR types from `Display.Mode.getSupportedHdrTypes()` (API 34+) or the
   deprecated `Display.getHdrCapabilities()` (API 28 to 33), the luminance figures of `HdrCapabilities` and, on API
   34+, `hdrSdrRatio`. A display that does not report `isHdr` gets no HDR types.
2. On an HDR display the Activity sets `window.colorMode = COLOR_MODE_HDR`, the surface gets buffer format
   `RGBA_1010102`, and on API 35+ `SurfaceView.setDesiredHdrHeadroom(8.0)`.
3. `OvtNative.setDisplayCaps()` passes the figures to native code, which logs one diagnostic line (tag `OVT`,
   `display: api=… hdr_types=[…] … encoding=…`) and picks the encoding: `Pq10` when HDR10 or HDR10+ is
   supported, else `Hlg10` when HLG is, else SDR.
4. `surfaceCreated` calls `OvtNative.surfaceAttach()`. Native code (`android/app/src/main/cpp/hdr_surface.cpp`)
   tags the window with `ANativeWindow_setBuffersDataSpace`, creates an EGL window surface (RGB10A2 and
   `EGL_EXT_gl_colorspace_bt2020_pq`/`_hlg` when the driver has them, RGBA8 for SDR), tags the window again
   (Android's `eglCreateWindowSurface` resets the dataspace), logs `OVT_HDR dataspace=<name>` and draws the test
   pattern. `surfaceChanged` and `surfaceRedrawNeeded` redraw it; `surfaceDestroyed` releases everything.
5. On API 34+ an `HdrSdrRatioChangedListener` forwards the live headroom with `OvtNative.setHeadroom()`.

| Encoding | Dataspace (field constants) | NDK name | Log line |
|---|---|---|---|
| `Pq10` | `STANDARD_BT2020 \| TRANSFER_ST2084 \| RANGE_FULL` | `ADATASPACE_BT2020_PQ` | `dataspace=BT2020_PQ` |
| `Hlg10` | `STANDARD_BT2020 \| TRANSFER_HLG \| RANGE_FULL` | `ADATASPACE_BT2020_HLG` | `dataspace=BT2020_HLG` |
| SDR | `STANDARD_BT709 \| TRANSFER_SMPTE_170M \| RANGE_LIMITED` | `ADATASPACE_BT709` | `dataspace=SRGB` |

If tagging a surface with an HDR dataspace fails, native code logs
`RND-0050 ANativeWindow_setBuffersDataSpace failed (<rc>); HDR output disabled for this session` once, falls back
to SDR for the rest of the process and logs `dataspace=SRGB`. A display without a 10-bit EGL config also falls back
to SDR (without RND-0050). Spec question #436 records where this differs from the text of the spec.

## The test pattern

Drawn with plain GLES scissored clears (no shaders) from the rectangle list of
`android/app/src/main/cpp/hdr_pattern.cpp`; libplacebo replaces it in WI-A.401. On a black background:

- top: a grey ramp of 32 equal steps of the 10-bit code value, 0 to 1023;
- middle: grey patches at 100, 203, 1000, 4000 and 10000 cd/m², each labelled below with its value;
- bottom: colour bars (white, yellow, cyan, green, magenta, red, blue) at 203 cd/m² per channel in BT.2020.

Full-range PQ codes of the patches: 100 → 520, 203 → 594, 1000 → 769, 4000 → 923, 10000 → 1023 (chapter 10
§10.4.4; checked by the host test `ovt_tests_android_hdr`). On an HLG display everything at or above 1000 cd/m² is
code 1023 and 203 cd/m² is 75 % (BT.2408). On an SDR display 203 cd/m² is white and everything brighter clips to
white.

## Checking a device

```sh
adb install -r app-foss-<abi>-debug.apk
adb logcat -c
adb shell am start -n org.openvideotools.studio/.MainActivity
adb logcat -d -s OVT_HDR          # dataspace=BT2020_PQ on an HDR10 panel, dataspace=SRGB on an SDR one
adb logcat -d -s OVT | grep -E 'display:|DisplayCaps|RND-0050'
adb shell dumpsys SurfaceFlinger | grep -i -A2 dataspace   # the HdrSurfaceView layer's dataspace
```

On the HDR device, compare the 203 and 1000 cd/m² patches (photometer if available, eyeball otherwise): the
1000 cd/m² patch must be clearly brighter, and the 203 cd/m² patch should match the white of the system UI.
Also check that the header band with the title `OVT Studio` is drawn above the surface (spec question #436 item 2).

## Device results

None recorded yet. WI-0.052 acceptance tests 1 and 3 need the `android-hdr` device (chapter 14 §14.9.2) or the
`chromebook-hdr` device; record here, per device: model, Android version and build, `supportedHdrTypes`,
`hdrSdrRatio`, the `OVT_HDR` line, a `dumpsys SurfaceFlinger` excerpt or screenshot showing the layer dataspace
`BT2020_PQ`, and the photometer or eyeball comparison of the 203 and 1000 cd/m² patches.
