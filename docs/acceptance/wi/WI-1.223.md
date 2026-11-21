# WI-1.223 acceptance record (part a)

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0. Date: 2026-11-21. Spec version: 1.0-rc45.
Commit: the head of the WI-1.223a pull request.

WI-1.223 is split (§16.2.6): part a carries acceptance tests 2 and 3 (box level), part b (the sync-sample probe and
`ovt_container_hdr_metadata`, after WI-1.217/1.218) carries tests 1 and 4.

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

## Test 2: `hdr_hlg_bt2020` is HLG

Automated: `AT2: hdr_hlg_bt2020 is HLG` (`[container][hdr][needs-corpus]`). The entry is `manual_download`
(nightly only, #517), so it skips here and in the PR lane. The same rule on synthetic bytes: `colr nclx 9/18/9 is
HLG at box level` (fast lane).
