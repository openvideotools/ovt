<!--
SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Screenshot tests (chapter 14 §14.10.2)

`ovt_studio_screenshots.cpp` is the harness. It renders one scene of OVT Studio offscreen and writes a PNG:

```sh
build/linux-x64-debug/app/tests/screenshots/ovt_studio_screenshots --scene main_window --theme dark --out /tmp/shots
build/linux-x64-debug/app/tests/screenshots/ovt_studio_screenshots --list-scenes
```

The window is 1600×1000 at 96 DPI and scale factor 1 (the harness sets `QT_SCALE_FACTOR=1` and `QT_FONT_DPI=96`),
on the `offscreen` platform unless `QT_QPA_PLATFORM` says otherwise. The output is `<out>/<scene>_<theme>.png`, an
opaque 8-bit RGB PNG, byte-identical between runs. Settings go to a temporary directory, never the user's.

Reference images live in `<platform>/<scene>_<theme>.png` here (Git LFS, §14.10.2) and are added with the scenes
(WI-1.576). Compare a render with its reference:

```sh
python -I tools/imgdiff.py --mode ui app/tests/screenshots/linux/main_window_dark.png /tmp/shots/main_window_dark.png --diff /tmp/diff.png
```

Exit 0 = match, 1 = differ (`--diff` writes `a | b | 4× difference` side by side), 2 = input error. The rule is
SSIM (grey, 7×7) ≥ 0.995 **and** no 8×8 tile with mean |Δ| above 12/255.

**Adding a scene:** add a row to `kScenes` in the harness. A scene that needs a fixture file or scripted state gets
its own code path there. Chrome scenes run with `OVT_RENDER_STUB=1`; renderer scenes use lavapipe.

**Fonts:** §14.10.2 requires the bundled `app/resources/fonts/Inter-Variable.ttf` (or `OVT_TEST_FONT`). The file is
not in the tree yet (its licence, SIL OFL 1.1, is a new licence type for `tools/license_policy.toml` and needs the
owner's decision), so the harness warns and uses Qt's default font. References must not be committed before the font
is bundled.
