# WI-A.403 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, clang 18.1.3, Mesa (llvmpipe, EGL surfaceless).
Date: 2026-11-17. Spec version: 1.0-rc41. Commit: the head of the WI-A.403 pull request.

Both acceptance tests need the Android emulator (or a device) with the APK built by the `android-build` CI job, which
the cloud container that implemented the WI does not have (no Android SDK, NDK or emulator can be downloaded there).
They are owed until the emulator smoke of #487 lands, with the steps below.

## Automated checks that do run

CTest `ovt_tests_android_gl_share` (label `fast`, built on non-Android configurations whose host has EGL and GLES;
its cases skip without an EGL display) runs `android/app/src/main/cpp/gl_share.cpp` on the host's EGL. A plain
GLES 3 context stands in for Qt's:

- `GlShareContext::create()` fails without a current context, and succeeds from the current one with Qt's config
  or with the config looked up from the context; `Scope` makes the shared context current and restores the host's
  context and surfaces.
- Acceptance test 2, host half: 300 frames, each drawn on the shared context as 16 scissored stripes and fenced
  (`glFenceSync` + `glFlush`, as `OffscreenGpu::end_frame()` does), then waited on from the host context
  (`GlShareContext::wait_on_current`, as `PictureHost` does) and read back through the host context's own
  framebuffer. Every frame's checksum equals the frame drawn (no partially drawn texture).

```console
$ ./ovt_tests_android_gl_share
All tests passed (328 assertions in 3 test cases)
```

The same binary is clean under `-fsanitize=address,undefined` with `detect_leaks=1` and `tests/asan.supp`.

`app/src/picture_host.cpp` and `app/src/picture_host_android.cpp` passed `-fsyntax-only` with the project's
warning flags and `-Werror` under GCC 13 and clang 18, against the Qt 6.8.3 headers, both as the desktop build and
with the Android branches enabled; clang-tidy reports nothing on the changed files. The real NDK build is the
`android-build` CI job, which builds the renderer into the APK from this WI on.

## Emulator half: the in-app self-check

The app cannot show a picture on Android through its UI yet. No decoder feeds PictureHost (WI-2.171), and an opened
file does not reach `Project` (see the WI-A.451 record). So debuggable APKs carry `app/src/picture_selftest.cpp`,
which runs both tests inside the app when the system property `debug.ovt.picture_selftest` is `1`. It renders a
PictureHost through `QQuickRenderControl` on Qt's OpenGL ES backend (so through `Interop::GlTexture` and the fence),
reads the Qt Quick window back, and compares it with an `Interop::None` VideoSurface of the same picture (the image
`SdrPng8` encodes, as in WI-1.405's desktop test). Release APKs never run it (`ApplicationInfo.FLAG_DEBUGGABLE`).

From `android/`, with the API 36 emulator running (`-gpu host`):

```console
./gradlew :app:assembleFossDebug :app:installFossDebug
adb shell setprop debug.ovt.picture_selftest 1
adb logcat -c
adb shell am start -W -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity
sleep 60
adb logcat -d | grep -E "OVT|ovt_render|UnsatisfiedLink|FATAL|dlopen"
adb shell setprop debug.ovt.picture_selftest 0
```

## Test 1: EMBEDDED frame on the emulator equals `SdrPng8` within 0.1 %

Self-check line: `OVT: picture-selftest AT1 backend=OpenGL interop=gl-texture size=1920x1080 differ=<n>/2073600
... result=PASS` (frame 0 of sdr709_ramp, built from its pattern definition; pass when at most 0.1 % of the pixels
differ by more than 1 per channel, the interop is `gl-texture` and PictureHost reports no error).

Result: owed (Mac Mini run).

## Test 2: 300 frames with the fence path show no partially rendered texture

Host half above. Self-check line: `OVT: picture-selftest AT2 interop=gl-texture frames=300 ... bad=0 ...
result=PASS`. It presents 300 different 320x180 pictures (moving diagonal stripes) one after another, with
dithering off in both renders, and a frame is bad when any pixel differs from its `Interop::None` render by more
than 1 per channel. A frame sampled before the surface finished it would show the previous picture there. The ±1
tolerance replaces "checksum equal" because the GL and Vulkan backends of libplacebo may round differently; `exact`
counts the frames whose bytes are identical as well.

Result: owed (Mac Mini run).
