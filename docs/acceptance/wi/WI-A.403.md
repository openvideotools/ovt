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

## Test 1: EMBEDDED frame on the emulator equals `SdrPng8` within 0.1 %

Not yet done: needs the emulator. Steps: install the foss debug APK on the API 36 emulator (`-gpu host`, see
project notes on SwiftShader), open `sdr709_ramp.mp4`, show frame 0 in the Picture panel, check that the status
strip reports `interop = gl-texture` (PictureHost's `interop` property), screenshot the PictureHost item and compare
it with the offscreen `SdrPng8` export of frame 0 (chapter 10 §10.8; ≤ 0.1 % pixels differ).

## Test 2: 300 frames with the fence path show no partially rendered texture

Host half above. Device half not yet done: needs the emulator. Steps: play 300 frames with `OVT_RENDER_GL_FINISH`
unset and compare each presented frame's checksum with an offscreen render of the same frame.
