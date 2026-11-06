# WI-0.563 acceptance record

HM getter dumper (`hm_cu_dump`), the FFmpeg motion-vector exporter (`ffmpeg_mvs`) and the JM/HM trace builds.
Date: 2026-11-06. Spec version: 1.0-rc4. Machine: Linux x86-64 cloud container (Ubuntu 24.04, FFmpeg 6.1.1 from the
distribution, libx264 0.164), not the oracle image. The image build is owed (see "Not verified here").

## Acceptance test 3: `ffmpeg_mvs` on an H.264 stream with P frames

Command: `python3 -I tests/oracles/test_ffmpeg_mvs.py <build>/ffmpeg_mvs`, also run as the CTest test
`oracle_ffmpeg_mvs` (`cmake -S tests/oracles -B b -DOVT_ORACLE_FFMPEG_MVS=ON`). Output: `ffmpeg_mvs: ok`, exit 0.

The stream is a 176x144 baseline libx264 encode of a lavfi test pattern, because `jvt_BA1_Sony_D` is not in this
container's cache (the test uses it when `OVT_CORPORA_DIR` has it). It checks: at least one row for every P frame
(picture types from `ffprobe`), `motion_scale == 4` on every row, the header row, the `--qp` CSV (16x16 blocks), a
byte-identical second run, `normalize/ffmpeg_mvs.py` accepting both CSVs, exit 2 on usage and exit 1 on a missing file.
A manual run on a 128x64, 12-frame, 2-B-frame libx264 clip gave 728 rows, all with `motion_scale` 4, and 384 QP rows.

## Acceptance test 1: `hm_cu_dump` on `jctvc_AMP_A_Samsung_7`

Not run. `vcgit.hhi.fraunhofer.de` (the pinned HM-18.0 source) is not reachable from this container (HTTP 403 from the
proxy), so the HM-18.0 libraries could not be built. `tests/oracles/test_hm_cu_dump.py` implements the test (AMP
partition sizes 4 to 7 appear; the cells satisfy `hm_grid_map.json`; two runs are identical; `normalize/hm_grid.py`
accepts the dump) and runs in the oracle image, where `hm_cu_dump` is on the path.

What was checked: `hm_cu_dump.cpp` was compiled and linked, with only mechanical renames for the older API, against an
unofficial GitHub mirror of HM 16.0 (`ChristianFeldmann/libHM`), which has the same `TDecTop` and `TComDataCU`
getters. It was not executed. The HM-18.0 differences it was written against (from memory of `TAppDecTop.cpp` of 16.20
and later): `byteStreamNALUnit(bytestream, nalu.getBitstream().getFifo(), stats)`, one-argument `read(nalu)`,
`pic->getCtu()` and `getNumberOfCtusInFrame()`. These are what the first image build will confirm.

## Acceptance test 2: `ldecod` TRACE on an MBAFF stream

`build_jm.sh` already builds `ldecod` with `TRACE 1` (WI-0.554). `smoke_test.sh` now encodes an MBAFF stream with
`x264 --interlaced` and requires `trace_dec.txt` to contain `mb_field_decoding_flag`. Not run here (no JM).

## Deviations

- `ffmpeg_mvs --qp <file>` names the QP CSV on the command line, and the MVs are enabled with
  `export_side_data |= AV_CODEC_EXPORT_DATA_MVS` instead of `flags2 +export_mvs`: spec question #181.
- `ffmpeg_mvs.c` is built by `build_ffmpeg_oracle.sh` against the image's FFmpeg; the CMake target is opt-in
  (`OVT_ORACLE_FFMPEG_MVS`, off by default) so the main build never links a machine's FFmpeg.
- `hm_cu_dump` also emits `getCUTransquantBypass`, `getTransformSkip_Y` and `chroma_format`; every getter of
  `hm_grid_map.json` is present. `getCbf_*` is the bit at the transform-unit leaf (HM stores one bit per depth).

## Not verified here

- The oracle image build (`build_hm.sh`, `build_ffmpeg_oracle.sh`, the Dockerfile stages, `oracle-smoke-test`):
  Docker is not available on the Mac Mini and hosted runners are off, the same debt as #155. It is owed to
  `oracle-image.yml` once hosted runners return, or to any Linux host that reaches `vcgit.hhi.fraunhofer.de`.
- Running `hm_cu_dump` against HM-18.0 (acceptance test 1).
