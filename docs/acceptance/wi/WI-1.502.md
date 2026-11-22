# WI-1.502 acceptance record (`ovt info`)

Machine: cloud container, Ubuntu 24.04 x86_64 (4 vCPU), GCC 13.3.0, preset `linux-x64-debug`. Date: 2026-11-22.
Spec version: 1.0-rc46.

## Test 1: `color.transfer == 16` on `syn_h265_main10_pq_hdr.mp4` — handed off

`tracks[].color` comes from `ovt_container_hdr_metadata`, which is still `OVT_E_UNSUPPORTED` on main (the C API
wiring is WI-1.223b, after WI-1.218). Coordinator ruling (2026-11-22, spec pass 27, #783): `ovt info` omits
`tracks[].color` and adds one Info diagnostic `CLI-0012`; the acceptance line moves to WI-1.223b. The omit path is
covered by `info: unsupported colour and index views are omitted with an Info diagnostic`.

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
`CLI-0012` (`info: unsupported colour and index views are omitted ...`).
