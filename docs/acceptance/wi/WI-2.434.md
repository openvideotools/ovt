# WI-2.434 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (Intel Xeon @ 2.30 GHz, 4 cores), GCC 13.3.0, pinned libplacebo 7.349.0
(built from source with the two `third_party/patches/libplacebo` patches), Mesa 25.2.8 lavapipe (llvmpipe, LLVM 20.1.2).
Date: 2026-11-16. Spec version: 1.0-rc38. Spec questions: #644 (intra strokes), #645 (AT1 viewport, timer,
K = 9), both accepted as proposed.

Both acceptance tests are automated in `render/tests/test_overlay_arrows.cpp`:

| AT | Test | Tags | Where it runs |
|---|---|---|---|
| 1 | `render_test_arrows: 518,400 arrows at 4K (AT1)` | `[.gpu]` | GPU runners only (M1 / Mac Mini) |
| 1 (golden half, 1080p viewport) | `render_test_arrows: synthetic_4k_mv golden (§10.9.2)` | `[fast][vulkan]` | every CI platform |
| 2 | `render_test_arrows: a 1-sample MV is a 2x2 dot (AT2)` | `[fast][vulkan]` | every CI platform |

## Fixture

`render/tests/fixtures/synthetic_4k_mv.fb` is generated (gitignored, 45 MB, not in the fast corpora). The tests read it
when present and otherwise build its in-memory twin (`make_synthetic_mv`), which has the same MotionVectors in the same
order. Both give the same golden:

```console
$ python3 tools/gen_fixture_fb.py --kind mv --size 4k --out render/tests/fixtures/synthetic_4k_mv.fb
$ sha256sum render/tests/fixtures/synthetic_4k_mv.fb
a13988b49df005f249ec232a0d50a28ed7ba9738264de99dc01dab32b6ccd2be  render/tests/fixtures/synthetic_4k_mv.fb   # = manifest.json
$ ./render/tests/ovt_tests_render "*synthetic_4k_mv golden*,*(AT1)"
bench_overlay_4k_mv_arrows arrow_pass_ms=337.831 (render+readback 400.367 ms with arrows, 62.5354 ms without)
All tests passed (2121 assertions in 3 test cases)
```

(The same run without the file, using the twin, also passes against the committed golden.)

## Golden storage

The stored-deflate PNG of the 1920×1080 golden is 6 MB, so it is committed gzip-compressed as
`tests/golden/render/synthetic_4k_mv.png.gz` (30 KB, `gzip -n -9`, reproducible: rewriting it with `OVT_UPDATE_GOLDEN=1`
gave the same SHA-256 `6ec3b7f9…669f`).

## Lavapipe run of the whole render binary

```console
$ ./render/tests/ovt_tests_render
All tests passed (12801 assertions in 104 test cases)
```

The lavapipe arrow time above is a software rasteriser on 4 cores; the FR-BLK-018 budget applies to the M1 run of AT1
and is gated by WI-2.438 (`bench_overlay_4k_mv_arrows`).

## Pending: MoltenVK on the Mac Mini

`ctest --preset macos-arm64-debug -R render_test_arrows` plus the hidden AT1 test
(`./render/tests/ovt_tests_render "[.gpu]"`) on the Mini, recorded here when run.
