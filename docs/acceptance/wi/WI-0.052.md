# WI-0.052 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, clang 18.1.3, OpenJDK 21. Date: 2026-11-14.
Spec version: 1.0-rc24. Commit: the head of the WI-0.052 pull request.

All three acceptance tests run on an Android device or emulator, which the cloud container that implemented the
WI does not have (no Android SDK, NDK or emulator can be downloaded there). They are owed, with the commands of
`docs/platforms/android-hdr.md` ("Checking a device"), and the results go into that document's "Device results".

## Automated checks that do run

CTest `ovt_tests_android_hdr` (label `fast`, built on every non-Android configuration) checks the
platform-independent half: the encoding rule of chapter 10 §10.4.3, the dataspace values against the NDK literals
(163971072, 168165376, 281083904), the `OVT_HDR` log names, the PQ codes of chapter 10 §10.4.4 (1, 100, 203,
1000, 4000, 10000 cd/m² → 153, 520, 594, 769, 923, 1023), HLG and SDR signals, and the pattern layout (every patch
present, inside the surface, deterministic).

```console
$ ./ovt_tests_android_hdr
All tests passed (52 assertions in 7 test cases)
```

In the container, the Kotlin sources compiled with `kotlinc` 2.0.21 `-Werror` against the API 36 `android.jar`
(Robolectric `android-all` 16) with stub `AppCompatActivity`, `QtQuickView` and `R` classes, and the JNI
descriptors registered in `ovt_jni.cpp` matched `javap -s` of the compiled `OvtNative`. `hdr_surface.cpp` and
`ovt_jni.cpp` passed `-fsyntax-only -Werror` with GCC 13 and clang 18 against Mesa's EGL/GLES headers and stub
NDK headers. The real NDK build is the `android-build` CI job (WI-0.034, PR #412) once it is on `main`.

## Test 1: 1000-nit patch brighter than 203-nit on `android-hdr`, `dumpsys SurfaceFlinger` shows `BT2020_PQ`

Not yet done: needs the `android-hdr` reference device.

## Test 2: emulator falls back to SRGB without a crash, logcat `OVT_HDR dataspace=SRGB`

Not yet done: needs the API 36 emulator. Expected: `adb logcat -d -s OVT_HDR` prints `dataspace=SRGB`, and the
`OVT` tag shows `display: api=36 hdr_types=[] … encoding=SRGB`.

## Test 3: `adb logcat -d | grep OVT_HDR` prints `dataspace=BT2020_PQ` on `android-hdr` or `chromebook-hdr`

Not yet done: needs either device.
