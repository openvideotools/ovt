# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare modes UI (WI-2.486; chapter 11 section 11.3.2): acceptance tests 1 and 2 through the test IPC.

Needs the IPC runner of WI-1.465 (``ovt-studio --test-ipc``): the tests are skipped when the application is not found
or does not serve the IPC yet (the ``studio`` fixture of conftest.py). Acceptance test 4 (SelectionB with a manual
offset) is covered by test_compare_controller.cpp and tst_picture.qml; acceptance test 3 (first mismatch on
``syn_metrics_1080p_bitflip_f37``) is skipped here until its inputs exist: the core compare job reaches the GUI with
WI-3.450 and ``tests/corpora/compare_expected.json`` is produced with the corpus recipe 52.

Object names used (commands of chapter 14 section 14.10.3): ``compareController`` (``mode``, ``splitPosition``,
``hasB``), ``compareSetAsB``, ``compareDivider`` and ``compareDividerTrack``.
"""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from conftest import REPO, IpcError, Studio

SAMPLE = REPO / "tests" / "corpora" / "patterns" / "hlg_patches.mp4"


@pytest.fixture
def compare_studio(studio: Studio) -> Iterator[Studio]:
    """A studio with a file open as A and a memory snapshot of its frame set as B."""
    if not SAMPLE.exists():
        pytest.skip(f"{SAMPLE} not found")
    try:
        studio.open(SAMPLE)
        studio.wait_idle()
        studio.click("compareSetAsB")
        studio.wait_idle()
        if not studio.get("compareController.hasB"):
            pytest.skip("no picture was decoded (the decoder path of WI-2.171 is not in this build)")
    except IpcError as err:
        pytest.skip(f"the test IPC cannot drive the compare controls yet: {err}")
    yield studio


def test_c_cycles_modes_in_documented_order(compare_studio: Studio) -> None:
    """AT1: Given A and B open, When C is pressed 6 times, Then the modes cycle in the documented order."""
    studio = compare_studio
    assert studio.get("compareController.mode") == 0
    seen = []
    for _ in range(6):
        studio.key("C")
        seen.append(studio.get("compareController.mode"))
    assert seen == [1, 2, 3, 4, 5, 6]  # Side by side, Split V, Split H, Overlay, Difference, Flicker
    studio.key("C")  # the seventh press wraps
    assert studio.get("compareController.mode") == 0
    studio.key("Shift+C")
    assert studio.get("compareController.mode") == 6


def test_divider_drag_sets_split_position(compare_studio: Studio) -> None:
    """AT2: Given Split mode, When the divider is dragged to 25 %, Then split_position == 0.25 +- 0.01."""
    studio = compare_studio
    for _ in range(2):
        studio.key("C")  # Single -> Side by side -> Split vertical
    assert studio.get("compareController.mode") == 2
    width = studio.get("compareDivider.width")
    studio.click("compareDividerTrack", int(width * 0.25), 5)
    assert studio.get("compareController.splitPosition") == pytest.approx(0.25, abs=0.01)


@pytest.mark.skip(reason="needs the core compare job in the GUI (WI-3.450) and tests/corpora/compare_expected.json")
def test_first_mismatch_bitflip_f37() -> None:
    """AT3: A = src_1080p_8bit, B = syn_metrics_1080p_bitflip_f37; Find first mismatch reports frame 37, plane Y,
    (611, 203), selects frame 37 and pins the probe there. The model half is test_compare_controller.cpp."""
    raise NotImplementedError  # written with the inputs
