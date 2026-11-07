<!--
SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
SPDX-License-Identifier: CC-BY-SA-4.0
-->
# ISOBMFF oracle goldens

`<corpus id>.<oracle>.json` for the `fast` ISOBMFF corpus files that `tools/fetch_corpora.py` can fetch
(chapter 14 §14.4.6-§14.4.9, WI-0.564). Oracles: `mp4box-diso` and `mp4ff-info` and `mediainfo-trace` and
`mp4boxjs` (`box-tree`), `mp4box-dts` (`sample-table`). `ORACLES.json` records the oracle versions and the
files an oracle refused. The goldens hold box metadata only, no media bytes.

Regenerate inside the oracle image, with the `fast` corpora fetched:

    python -I tools/fetch_corpora.py --tags fast --cache-dir "$OVT_CORPUS_CACHE"
    python -I tools/goldens/gen_isobmff.py generate --cache-dir "$OVT_CORPUS_CACHE"
    python -I tools/goldens/gen_isobmff.py check

`KNOWN_DIFFS.md` lists every disagreement between the oracles. Files in this folder that have no corpus id
(`ovt_min_structural.*`, `box_tree_main.cpp`, `gen_min_mp4.py`) belong to WI-0.093-era tests.
