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

The wire format is not in the spec yet (spec question #353): JSON lines over a Unix socket, request
`{"id":1,"cmd":"click","args":["timeline",37]}`, response `{"id":1,"ok":true,"value":…}` or
`{"id":1,"ok":false,"error":"…"}`. `test_ipc_client.py` tests the client against a fake server that follows it; WI-1.465
implements the real server against the same format.
