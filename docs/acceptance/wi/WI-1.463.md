# WI-1.463 acceptance record

Spec version: 1.0-rc41. Scope per chapter 16 §16.3: the macOS NATIVE host only (the Linux host is WI-4.450).

## Test 1: XDR Mac, `Ctrl+H` → `ScRgbLinear16F`; child window = item rect ±1 px over a 100-step splitter drag

Automated parts (every CI platform unless noted):

- Geometry: `WI-1.463 AT1 (geometry): the child window follows the item +-1 px during a 100-step splitter drag` in
  `app/tests/models/test_native_surface_host.cpp` (binary `ovt_tests_app_native_host`, offscreen platform, test
  backend). A main window with a left pane, a 6 px handle and a right pane holding the picture item is dragged in 100
  fractional steps; at every step the child `QWindow`'s geometry and `NativeSurfaceHost::geometry()` equal the item's
  scene rect ±1 px, and the NATIVE drawable is that rect × `devicePixelRatio`.
- `Ctrl+H` → HDR output requested: `Main::test_hdrOutputAction` in `app/tests/qml/tst_main.qml` (View ▸ HDR output
  and the toolbar toggle set `HdrView.hdrOutput`, only on an HDR-capable display; the button is lit only while the
  host reports an HDR encoding). The §11.4.3 key binding itself is covered by `Shortcuts::test_everyRowFiresOnce`.
- Surface side (macOS CI and the Mac Mini): `render_test_macos_metal: a MetalTexture surface retargets to NATIVE and
  back (WI-1.463)` in `render/tests/test_macos_metal.mm`: the surface the macOS PictureHost creates
  (`Interop::MetalTexture`) retargets to a real `CAMetalLayer` with `active_encoding() == ScRgbLinear16F`, presents,
  and retargets back to its offscreen texture.
  Run on the Mac Mini (Apple M4, macOS 15.8.2, `macos-arm64-debug`, commit 54878c4, 2026-11-17):
  `ovt_tests_render "render_test_macos_metal*"` passed (29 assertions in 5 test cases) and all 56 `[vulkan]` render
  cases passed (10 777 assertions), no skips. Log: project file
  `wi1463-pr703-render-mini-20261117T033557Z.txt`.

**Deferred manual record** (needs a macOS XDR display, §16.5.3; owed to Karl together with the WI-1.406 record, since
the Mac Mini's display is SDR). Steps:

1. `cmake --preset macos-arm64-debug && cmake --build --preset macos-arm64-debug --target ovt_studio`.
2. Start `OVT Studio.app` on the XDR display, open any file so that the Picture panel shows the picture area, and
   check the status bar reads `HDR headroom …×`.
3. Press `Ctrl+H`. Expected: the toolbar's "HDR output" button is lit, the Display Capabilities panel's output
   encoding row reads `ScRgbLinear16F`, and the picture area is a native child window (the stderr log has no
   `RND-0002`).
4. Drag the splitter between the left area and the Picture panel slowly across its range (about 100 pointer steps).
   Expected: the picture stays aligned with the panel (no gap or overlap wider than 1 px on any edge, checked on a
   screenshot at the start, middle and end of the drag).
5. Open the View menu over the picture. Expected: the picture switches to the SDR texture while the menu is open and
   back to HDR when it closes.

Record date, machine (model, macOS version, display), and the observations of steps 3–5.
