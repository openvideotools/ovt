<!--
SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
SPDX-License-Identifier: CC-BY-SA-4.0
-->

# GUI tests

How the GUI of OVT Studio is tested (chapter 14 §14.10; chapter 11 §11.9). WI-0.568 delivers the tooling; the tests
that use it come with WI-1.465 (the in-app server) and WI-1.576 (scenes and scripts).

| Layer | Where | Run by |
|---|---|---|
| C++ model tests (Catch2) | `app/tests/models/` | `ctest -L gui` |
| Qt Quick Test, `tst_*.qml` on the real QML module | `app/tests/qml/` (`tst_Smoke.qml` checks the runner itself) | `ovt_tests_app_qml`, `QT_QPA_PLATFORM=offscreen` |
| Screenshot harness and `imgdiff.py --mode ui` | `app/tests/screenshots/` | `gui_scripts_pytest` now, the screenshot job of WI-1.576 later |
| Interaction scripts over the test IPC | `app/tests/scripts/test_*.py` | `gui_scripts_pytest` |

## Running

```sh
python -m pip install -r tools/requirements-test.txt       # pytest, NumPy, scikit-image
cmake --build --preset linux-x64-debug
ctest --preset linux-x64-debug -L gui
```

On Linux the tests need no display (`offscreen`). `QT_QPA_PLATFORM` set by the caller wins.

## `tools/imgdiff.py`

```sh
python -I tools/imgdiff.py --mode ui         a.png b.png [--diff out.png]
python -I tools/imgdiff.py --mode hdr-parity a.png b.png [--diff out.png] [--p99-max 1.0] [--mean-max 0.3]
```

* `ui`: SSIM ≥ 0.995 and no 8×8 tile above 12/255 (§14.10.2).
* `hdr-parity`: linear-light ΔE ITP (BT.2124) per pixel; 16-bit PNGs are decoded as PQ (ST 2084, BT.2020), 8-bit PNGs as
  BT.1886 at 203 nits (BT.709 primaries; `--sdr-primaries bt2020` changes that). Prints mean, p99 and max; passes when
  p99 ≤ `--p99-max` (default 1.0) and, if given, mean ≤ `--mean-max` (§14.4.15 uses 1.0 and 0.3 for SDR, 0.5 for HDR
  pass-through).
* Exit 0 / 1 / 2 = match / differ / input error. `--json` prints one JSON object.
* The PNG reader and writer are in the script, because Pillow cannot read 16-bit RGB. Tests: `tools/tests/test_imgdiff.py`.

## Test IPC client

`app/tests/scripts/conftest.py` provides `ipc` (starts `ovt-studio --test-ipc <socket>` offscreen with
`OVT_RENDER_STUB=1` and a private `OVT_CONFIG_DIR`, connects, stops it afterwards) and `studio(ipc)` with the
§14.10.3 commands: `open`, `click`, `select_syntax_node`, `key`, `get`, `screenshot`, `wait_idle`. The binary comes from
`--ovt-studio <path>`, `$OVT_STUDIO` or `build/*/app/ovt-studio`; scripts skip when there is none.

The wire format is in chapter 14 §14.10.3: JSON lines over a Unix socket, request
`{"id":1,"cmd":"click","args":["timeline",37]}`, response `{"id":1,"ok":true,"value":…}` or
`{"id":1,"ok":false,"error":"…"}`. `test_ipc_client.py` tests the client against a fake server; `test_test_ipc.py` runs
every command against the real application.

## Test IPC server (WI-1.465)

`ovt-studio --test-ipc <socket>` listens on the Unix socket `<socket>` (`QLocalServer`, user-only permissions) and
answers one JSON line per request, in request order. Without the flag no socket exists. The server
(`app/src/testipc/`) is GPL app code compiled only with `-DOVT_ENABLE_TEST_IPC=ON` (default `OFF`, in every preset, so release bundles never
contain it; it also needs `OVT_BUILD_TESTS=ON`). Without it the `--test-ipc` option does not exist and the pytest
lane skips. The `gui` CI job turns it on. Commands:

| Command | Meaning |
|---|---|
| `open <path>` | `Project.openFile`; value `{fileCount, activeFile}`; an error if the project raised a diagnostic |
| `click <name>` / `click <name> <row>` / `click <name> <x> <y>` | click the centre, a row (`ListView.itemAtIndex`, or the timeline's `xOfFrame`), or item-local (x, y). Item-local points are mapped with `mapToScene` and sent through `QTest::mouseClick`, never as item-local coordinates |
| `select-syntax-node <path>` | calls `selectByPath(path)` of the object named `syntaxTree` (the Syntax panel provides it) |
| `key <sequence>` | `QKeySequence` portable text (`J`, `Ctrl+G`), sent to the window through `QTest::keyClick`, so shortcuts fire |
| `get <name>.<property>[.<key>…]` | a property; further segments read keys of maps, indexes of lists and properties of objects. A first segment that starts upper-case names a QML singleton (`get Project.fileCount`) |
| `screenshot <path>` | the window as PNG (`grabWindow`); value `{path, width, height}` |
| `wait-idle [timeout_ms]` | no running job (`JobsModel.activeCount == 0`) and no frame swapped for three 20 ms polls; error after the timeout (default 10 s) |

Objects are found by `objectName` (the QObject tree first, then the item tree, so delegates are found). A panel that
tests will drive needs an `objectName`.

`OVT_RENDER_STUB=1` makes the Picture panel's `PictureHost` create no libovt-render surface (its `interop` is `stub`
and it draws only its background colour), so chrome scenes and scripts do not depend on a GPU. Renderer scenes leave it
unset and run on lavapipe.

CI: the `gui` job of `.github/workflows/ci.yml` builds the GUI targets and runs `ctest --preset linux-x64-release -L
gui` (Catch2, Qt Quick Test, PictureHost under xvfb, the pytest lane). `app/tests/testipc` tests the dispatch and the
socket server against a small QML scene without the application.
