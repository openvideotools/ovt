# WI-1.406 acceptance record

Spec version: 1.0-rc24. Spec question: #470. WI-1.406 is split (§16.2.6): part a (this record) is the NATIVE target
and its automated tests; part b adds the manual EDR pattern tool (step 5) and the XDR record of test 1.

## Test 1: XDR display, NATIVE ScRgbLinear16F, 1000-nit patch ≥ 4× the 203-nit patch

Automated part: `render_test_macos_metal: NATIVE ScRgbLinear16F on an EDR CAMetalLayer (acceptance test 1, automated
part)` (`render/tests/test_macos_metal.mm`): a real CAMetalLayer through MoltenVK gives `active_encoding() ==
ScRgbLinear16F`, EDR on, `RGBA16Float`, the extended-linear colour space, and frames present. The colorimeter or
visual measurement on an XDR display is **owed** (hardware-bound, §16.5.3) and is recorded here by WI-1.406b.

## Test 2: SDR display, NATIVE requested → `Srgb8`

Automated: `render_test_native: encoding choice from the surface formats (§10.4.1, RND-0040)` (the rule, with
MoltenVK's formats), `render_test_native: SDR display falls back to Srgb8 on a NATIVE swapchain (acceptance test 2)`
(headless Vulkan surface), `render_test_macos_metal: SDR display falls back to Srgb8 (acceptance test 2)`
(CAMetalLayer, macOS). The `DisplayCaps.hdr_capable == false` half needs `display_probe()`, which WI-1.407 delivers
and tests (#470 item 2).
