<!--
SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
SPDX-License-Identifier: CC-BY-SA-4.0
-->
# CENC per-sample goldens

`<corpus id>.mp4ff-senc.json`: the IV and the subsample list of every `senc` sample of every track, read with mp4ff
(the module version of `tests/golden/isobmff/ORACLES.json`; `mp4ff-info` itself does not print them). They are the
oracle of the WI-1.221 acceptance test 1 (`cenc: mpegff_video_cenc_128iv ...`). Regenerate inside the oracle image,
with the corpus fetched:

    python -I tools/fetch_corpora.py --ids mpegff_video_cenc_128iv --cache-dir "$OVT_CORPUS_CACHE"
    (cd tests/oracles/mp4ff_senc && go run . "$OVT_CORPUS_CACHE/mpegff_video_cenc_128iv/video_cenc_128iv.mp4") \
        > tests/golden/cenc/mpegff_video_cenc_128iv.mp4ff-senc.json
