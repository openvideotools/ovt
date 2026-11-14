# WI-4.452 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (Intel Xeon @ 2.10 GHz), GCC 13.3.0, libplacebo 6.338 (distribution
package; CI builds the pinned 7.349), Mesa 25.2.8 lavapipe, FFmpeg 6.1.1 (distribution `ffmpeg`).
Date: 2026-11-14. Spec version: 1.0-rc24. Spec question: #473.

Both acceptance tests are automated (`render_test_measure: AT1 …` and `render_test_measure: AT2 …` in
`render/tests/test_measure.cpp`, tags `[fast][render][vulkan]`). They synthesise frame 0 of the pattern from its
`*.expected.json` instead of decoding the `.mp4`, as the WI-1.401/WI-1.403 render tests do, because the render tests
do not link a decoder. This record shows that the synthesised frame is the decoded frame, byte for byte.

## Synthesised frame 0 = decoded frame 0

```console
$ for p in pq_patches_1000 hlg_patches; do
    ffmpeg -v error -i tests/corpora/patterns/$p.mp4 -frames:v 1 -f rawvideo -pix_fmt yuv420p10le -y $p.f0.yuv
  done
$ python3 -I cmp.py tests/corpora/patterns .   # builds Y exactly as test_measure.cpp does, compares with the decode
pq_patches_1000 frame0: luma equal: True  chroma all 512: True  sha256(Y) 03d573b4cc423266
hlg_patches frame0: luma equal: True  chroma all 512: True  sha256(Y) 1a911d542cb64ad2
```

`cmp.py` fills luma with 64, writes each `patch_columns` / `patch_rects` entry with its limited code, and (HLG) the
ramp `floor(876 · x / 1919 + 64 + 0.5)` over `ramp_rect`, then compares with `numpy.array_equal`.

## Measured values (lavapipe)

```console
$ ./render/tests/ovt_tests_render "*AT1*" -s | grep -A2 'CHECK( m\.'
  9992.0f == Approx( 10000.0 )          max_cll_nits          (±1 %)
  10000.0f >= 4000.0                    max_cll_p9999_nits    (top band of Table 10.7-1)
  2551.97778f == Approx( 2550.666666667 ) fall_nits           (±1 %)
  0.9917f == Approx( 0.992131264 )      min_nits
$ ./render/tests/ovt_tests_render "*AT2*" -s | grep -A2 'CHECK( \(patch\|frame\)\.'
  203.125f == Approx( 203.0 )           75 % patch, fall and max_cll (±3 %)
  999.5f == Approx( 1000.00002924 )     whole frame max_cll   (±1 %)
  379.80515f == Approx( 379.965145397 ) whole frame fall      (±1 %)
```

All 6 `render_test_measure` cases pass (180 assertions, 1.9 s); the full `ovt_tests_render` binary passes
(48 cases). Determinism: the subsampling case measures the same frame twice and compares every field and the
histogram bit for bit.
