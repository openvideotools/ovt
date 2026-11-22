# WI-1.223 acceptance record (parts a and b)

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0. Date: 2026-11-21. Spec version: 1.0-rc45.
Commit: the head of the WI-1.223a pull request.

WI-1.223 is split (§16.2.6): part a carries acceptance tests 2 and 3 (box level), part b (the sync-sample probe and
`ovt_container_hdr_metadata`, after WI-1.217/1.218) carries tests 1 and 4. Part b's record: 2026-11-22, spec version
1.0-rc46, the head of the WI-1.223b pull request, same machine.

## Test 1: the ISOBMFF remux of `hdr_hdr10plus_dv81` is Dolby Vision 8.1 with HDR10+

Automated: `AT1: hdr_hdr10plus_dv81 is Dolby Vision 8.1 with HDR10+` (`[container][hdr][needs-corpus]`, through
`ovt_container_hdr_metadata`). The same rules on synthetic bytes, fast lane: `AT1 shape: Dolby Vision 8.1 with HDR10+
T.35 and an RPU in sample 0`.

`hdr10plus_tool extract` finds no HDR10+ in the dovi_tool `regular.mkv` (it has DV RPUs, SEI 137 and SEI 144), so the
manifest entry records the injection (#769): the fetcher's `mp4box_dv_remux` post step extracts the HEVC stream,
injects `tests/corpora/synthetic/hdr10plus_metadata.json` with `hdr10plus_tool inject` and remuxes with
`MP4Box -add <stream>:dv-profile=8.1`. The PR-lane runners have neither MP4Box nor hdr10plus_tool, so there the entry
is unavailable and AT1 skips until a runner carries both tools. Local run (ffmpeg 6.1.1, hdr10plus_tool 1.7.2 built
from its repository, MP4Box 2.2.1):

```console
$ hdr10plus_tool extract regular.hevc -o x.json
Error: File doesn't contain dynamic metadata
$ hdr10plus_tool inject -i regular.hevc -j tests/corpora/synthetic/hdr10plus_metadata.json -o inj.hevc
Warning: mismatched lengths. video 259, HDR10+ JSON 60
Metadata will be duplicated at the end to match video length
$ MP4Box -for-test -add inj.hevc:dv-profile=8.1 -new hdr_hdr10plus_dv81.mp4
$ OVT_CORPORA_DIR=corpora ovt_tests_container "AT1: hdr_hdr10plus_dv81*"
All tests passed (9 assertions in 1 test case)
$ ovt info -f json hdr_hdr10plus_dv81.mp4   # result.tracks[0].color
{"primaries": 9, "transfer": 16, "matrix": 9, "full_range": 0, "max_cll": 1000, "max_fall": 400,
 "mdcv_max_luminance": 10000000, "has_hdr10plus": true, "has_dolby_vision": true}
```

## Test 4: a 2 GB file is probed with at most 16 MiB read

Automated, fast lane: `AT4: a 2 GB file is probed with at most 16 MiB read`. A sparse 2 GiB file (an `mdat` with a
64-bit size, eight 5 MiB sync samples 256 MiB apart) opened through `ovt_container_open`; the `bytes_read` delta of
`ovt_container_hdr_metadata` (sample table expansion plus probe) is at most 16 MiB, and three samples are probed (a
fourth would take the reads over the default `max_bytes`, #790). Passes.

## Test 3: `syn_h265_main10_pq_hdr_mismatch` raises CNT-5001

Automated: `AT3: syn_h265_main10_pq_hdr_mismatch raises CNT-5001 (colr BT.709 over a BT.2020 SPS)`
(`[container][hdr][needs-corpus]`, `core/tests/container/hdr_summary_test.cpp`). The manifest entry has no `fast`
tag, so the test skips in the PR lane. Local run on a file regenerated with the recipe 23 command over a 10-frame
BT.2020/PQ Main 10 x265 stream (x265 3.5, MP4Box 2.2.1, ffmpeg 6.1.1; not the manifest's sha256):

```console
$ ffmpeg -f lavfi -i testsrc2=size=320x180:rate=30 -frames:v 10 -pix_fmt yuv420p10le -strict -1 src.y4m
$ x265 --input src.y4m --output-depth 10 --profile main10 --colorprim bt2020 --transfer smpte2084 \
    --colormatrix bt2020nc --hdr10 --master-display "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)" \
    --max-cll 1000,400 --output syn_h265_main10_pq_hdr.265
$ MP4Box -add syn_h265_main10_pq_hdr.265:colr=nclc,1,1,1 -new syn_h265_main10_pq_hdr_mismatch.mp4
$ OVT_CORPORA_DIR=corpora ovt_tests_container "AT3: syn_h265_main10_pq_hdr_mismatch raises CNT-5001 (colr BT.709 over a BT.2020 SPS)"
All tests passed (6 assertions in 1 test case)
```

Nightly on the PR branch (head 1cec190, 2026-11-21, `corpora-linux`, ubuntu-22.04, GCC 13, `OVT_REQUIRE_CORPORA=1`,
the manifest's recipe-23 stream generated in the oracle image):
<https://github.com/openvideotools/ovt/actions/runs/44926771714/job/136914466390>. Result: **pass** (AT3 is not among
the failed or skipped tests; the `hdr` label ran 2 tests: AT3 passed, AT2 failed as below).

## Test 2: `hdr_hlg_bt2020` is HLG

Automated: `AT2: hdr_hlg_bt2020 is HLG` (`[container][hdr][needs-corpus][slow]`). The same rule on synthetic bytes:
`colr nclx 9/18/9 is HLG at box level` (fast lane, passes).

**Status: pending real asset.** The entry is `manual_download` and `unverified` with no known URL (#517), so no
fetcher can get it. In the nightly run above, AT2 failed with the asset missing, which `OVT_REQUIRE_CORPORA=1`
reports as a failure. The other 13 failures in that job also fail on main's scheduled nightly of the same day
(run 44893316029). Per the coordinator's ruling (2026-11-21), part a merges with AT2 recorded as pending. The record
of the real `hdr_hlg_bt2020` comes from the first nightly after the BBC R&D HLG clip is placed in the corpora by hand.
