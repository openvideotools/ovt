# WI-1.502 acceptance record (`ovt info`)

Machine: cloud container, Ubuntu 24.04 x86_64 (4 vCPU), GCC 13.3.0, preset `linux-x64-debug`. Date: 2026-11-22.
Spec version: 1.0-rc46.

## Test 1: `color.transfer == 16` on `syn_h265_main10_pq_hdr.mp4`

Handed to WI-1.223b (#783) and closed there: `ovt_container_hdr_metadata` now answers for ISOBMFF, so `ovt info`
fills `tracks[].color` and adds no `CLI-0012` for it. Automated on a synthetic file (fast lane): `info: the colour view
comes from the HDR metadata; a PQ colr gives transfer 16` (`cli/tests/test_info.cpp`). The CLI tests link no corpus
support, so the corpus line is a manual record (2026-11-22, cloud container, GCC 13.3.0). The file is the recipe 10
commands (x265 3.5, ffmpeg 6.1.1) over a locally generated source, so not the manifest's sha256:

```console
$ ovt info -f json syn_h265_main10_pq_hdr.mp4   # status, result.tracks[0].color, diagnostics
OK {'primaries': 9, 'transfer': 16, 'matrix': 9, 'full_range': 0, 'max_cll': 1000, 'max_fall': 400,
    'mdcv_max_luminance': 10000000, 'has_hdr10plus': False, 'has_dolby_vision': False} []
```

The same file through the C API: `FR-CNT-102: syn_h265_main10_pq_hdr is HDR10 with MaxCLL 1000 / MaxFALL 400`
(`core/tests/container/hdr_summary_test.cpp`, `[needs-corpus]`), passes locally.

## Test 2: a 1-hour file in ≤ 200 ms

Not run on an M1 (no such machine here). A header-only 1-hour file (216000 frames at 60 fps, constant-size `stsz`,
490 bytes) on the cloud container:

```console
$ time build/linux-x64-debug/cli/ovt info h1.mp4 > /dev/null
real    0m0.026s
```

Automated sanity bound (< 2 s, any CI machine): `info: a 1-hour header-only file reports quickly`. The ≤ 200 ms
figure on an M1 is still to be recorded on the Mac Mini (hardware-bound, §16.5.3).

## Test 3: `-f csv` exits 2

Automated: `info: -f csv is a usage error` (`cli/tests/test_info.cpp`).

## `--full`

`ovt_stream_index_summary` is a stub until WI-1.125; `--full` omits `index_summary` and adds an Info diagnostic
`CLI-0012` (`info: an unsupported index view is omitted with an Info diagnostic`).
