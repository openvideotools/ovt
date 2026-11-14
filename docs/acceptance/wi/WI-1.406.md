# WI-1.406 acceptance record

Spec version: 1.0-rc24. Spec question: #470 (scRGB swapchain pair at libplacebo 7.349; AT2 `DisplayCaps` half).

## Test 1: XDR display, NATIVE ScRgbLinear16F, 1000-nit patch ≥ 4× the 203-nit patch (OWED, hardware-bound)

Automated part (macOS CI and the Mac Mini): `render_test_macos_metal: NATIVE ScRgbLinear16F on an EDR CAMetalLayer
(acceptance test 1, automated part)` in `render/tests/test_macos_metal.mm`: a real CAMetalLayer through MoltenVK gives
`active_encoding() == ScRgbLinear16F`, EDR on, `RGBA16Float`, the extended-linear colour space, and frames present.

**Owed manual record** (needs a macOS XDR display, §16.5.3; not yet run):
`cmake --build --preset macos-arm64-debug --target ovt_manual_edr_pattern`, then
`build/macos-arm64-debug/render/tests/ovt_manual_edr_pattern --encoding scrgb --seconds 60`.
Record date, machine (model, macOS, display), the tool's output, and colorimeter readings of patch 4 (1000 nits) and
patch 3 (203 nits) with their ratio (pass ≥ 4.0) or the verdict "patch 4 brighter than the white strip below".

## Test 2: SDR display, NATIVE requested → `Srgb8`

Automated: `render_test_native: encoding choice from the surface formats (§10.4.1, RND-0040)` (the rule, with
MoltenVK's formats), `render_test_native: SDR display falls back to Srgb8 on a NATIVE swapchain (acceptance test 2)`
(headless Vulkan surface), `render_test_macos_metal: SDR display falls back to Srgb8 (acceptance test 2)`
(CAMetalLayer, macOS). The `DisplayCaps.hdr_capable == false` half needs `display_probe()`, which WI-1.407 delivers
and tests (#470 item 2).
