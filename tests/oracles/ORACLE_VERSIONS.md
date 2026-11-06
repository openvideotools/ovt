# Oracle versions

The tools of the oracle image `ghcr.io/openvideotools/ovt-oracles:<yyyymmdd>` (chapter 14 §14.2.6, built from
[`Dockerfile`](Dockerfile) by WI-0.554). Golden files print these versions in their header
(`# oracle: ffmpeg n7.1.1 ...`). Every source is a tag checked against its commit, an archive checked
against its SHA-256, or a hash-locked package; nothing floats, so a rebuild a week later reports the same
versions (`oracle_versions.py --check-pins` enforces it, the monthly `oracle-image` run compares reports).

## Changing a pin

1. Edit the pin in the build script, [`Dockerfile`](Dockerfile), [`requirements.in`](requirements.in) or
   [`package.json`](package.json), and the matching row below, in one PR that touches nothing else.
2. Python: `uv pip compile requirements.in --python-version 3.12 --python-platform x86_64-manylinux_2_28
   --index-url https://pypi.org/simple --extra-index-url https://download.pytorch.org/whl/cpu
   --index-strategy unsafe-best-match --emit-index-url --generate-hashes --no-header -o requirements.txt`
   (the PyTorch index serves the CPU-only `torch` and `torchvision` wheels). Node: `npm install --package-lock-only`.
3. The `oracle-image` workflow rebuilds the image on the PR and runs `oracle-versions` and the smoke test.
4. Goldens produced by the changed tool are regenerated in a follow-up PR (chapter 14 §14.4.0).

## Base

| Item | Pin |
|---|---|
| Base image | `ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852` |
| Ubuntu archive snapshot (all apt packages) | `20261101T000000Z` (<https://snapshot.ubuntu.com>) |
| Node.js | `v20.20.2`, `node-v20.20.2-linux-x64.tar.xz` sha256 `df770b2a6f130ed8627c9782c988fda9669fa23898329a61a871e32f965e007d` |
| Python | 3.12 from the snapshot; packages hash-locked in [`requirements.txt`](requirements.txt) |

## Tools

`Probe` runs in the image; `Expect` is the text the probe's output must contain. Tools that print no
version (or the same version for every release) report the stamp their build script wrote under
`$OVT_ORACLES_STAMPS`. `vmaf --version` prints `git describe --match '?.*.*' --always`, which never matches
the `v3.0.0` tag, so it reports the pinned commit's short hash.

<!-- oracle-versions:begin -->
| Tool | Version | Source | Licence | Probe | Expect |
|---|---|---|---|---|---|
| ffmpeg | n7.1.1 | <https://github.com/FFmpeg/FFmpeg> `db69d06eeeab4f46da15030a80d539efb4503ca8` | GPL-2.0-or-later (this build) | `ffmpeg -hide_banner -version` | `ffmpeg version n7.1.1` |
| ffprobe | n7.1.1 | as ffmpeg | GPL-2.0-or-later (this build) | `ffprobe -hide_banner -version` | `ffprobe version n7.1.1` |
| x264 | r3191 | <https://code.videolan.org/videolan/x264> `4613ac3c15fd75cebc4b9f65b7fb95e70a3acce1` | GPL-2.0-or-later | `x264 --version` | `0.164.3191 4613ac3` |
| x265 | 4.1 | <https://bitbucket.org/multicoreware/x265_git> `1d117bed4747758b51bd2c124d738527e30392cb` | GPL-2.0-or-later | `x265 --version` | `encoder version 4.1` |
| aomenc | 3.12.1 | `libaom-3.12.1.tar.gz` sha256 `9e9775180dec7dfd61a79e00bda3809d43891aee6b2e331ff7f26986207ea22e` (tag v3.12.1, `10aece4157eb79315da205f39e19bf6ab3ee30d0`) | BSD-2-Clause + AOM patent licence | `aomenc --help` | `3.12.1` |
| aomdec | 3.12.1 | as aomenc | BSD-2-Clause + AOM patent licence | `aomdec --help` | `3.12.1` |
| inspect | 3.12.1 | as aomenc, `CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1` | BSD-2-Clause + AOM patent licence | `cat "$OVT_ORACLES_STAMPS/inspect.version"` | `libaom v3.12.1` |
| dav1d | 1.5.1 | <https://code.videolan.org/videolan/dav1d> `42b2b24fb8819f1ed3643aa9cf2a62f03868e3aa` | BSD-2-Clause | `dav1d --version` | `1.5.1` |
| vmaf | 3.0.0 | <https://github.com/Netflix/vmaf> `17a67b238ce0539bdeafdc95961abac64fa16ea8`, `-Denable_float=true` | BSD-2-Clause-Patent | `vmaf --version` | `17a67b2` |
| MP4Box | v2.4.0 | <https://github.com/gpac/gpac> `5d70253ac94e5840be7b86054131dd753af63cc7` | LGPL-2.1-or-later | `MP4Box -version` | `GPAC version 2.4` |
| mp4dump | v1.6.0-641 | <https://github.com/axiomatic-systems/Bento4> `dc264854d1f76c370b65b18d9f303a95f7f21ab1` | GPL-2.0-or-later | `cat "$OVT_ORACLES_STAMPS/mp4dump.version"` | `Bento4 v1.6.0-641` |
| mp4encrypt | v1.6.0-641 | as mp4dump | GPL-2.0-or-later | `cat "$OVT_ORACLES_STAMPS/mp4encrypt.version"` | `Bento4 v1.6.0-641` |
| mp4fragment | v1.6.0-641 | as mp4dump | GPL-2.0-or-later | `cat "$OVT_ORACLES_STAMPS/mp4fragment.version"` | `Bento4 v1.6.0-641` |
| mp4ff-info | v0.48.0 | `go install github.com/Eyevinn/mp4ff/cmd/mp4ff-info@v0.48.0` (`go.sum` checked) | MIT | `mp4ff-info -version` | `v0.48` |
| mp4box.js | 1.5.0 | npm `mp4box@1.5.0`, [`package-lock.json`](package-lock.json) | BSD-3-Clause | `node -p "require(process.env.NODE_PATH + '/mp4box/package.json').version"` | `1.5.0` |
| node | v20.20.2 | see Base | MIT | `node --version` | `v20.20.2` |
| mediainfo | 24.01 | Ubuntu 24.04 snapshot | BSD-2-Clause | `mediainfo --Version` | `v24.01` |
| mkvinfo | 82.0 | Ubuntu 24.04 snapshot (`mkvtoolnix`) | GPL-2.0-or-later | `mkvinfo --version` | `v82.0` |
| mkvmerge | 82.0 | Ubuntu 24.04 snapshot (`mkvtoolnix`) | GPL-2.0-or-later | `mkvmerge --version` | `v82.0` |
| tsp | 3.45-4798 | `tsduck_3.45-4798.ubuntu24_amd64.deb` sha256 `0023689f76e75b64e45253771208bcf8ca95b91fbfd0900da097c729f3c39fde` | BSD-2-Clause | `tsp --version` | `3.45-4798` |
| tstables | 3.45-4798 | as tsp | BSD-2-Clause | `tstables --version` | `3.45-4798` |
| tsanalyze | 3.45-4798 | as tsp | BSD-2-Clause | `tsanalyze --version` | `3.45-4798` |
| gstreamer-codecparsers | 1.24.2 | Ubuntu 24.04 snapshot (`libgstreamer-plugins-bad1.0-dev`) | LGPL-2.1-or-later | `pkg-config --modversion gstreamer-codecparsers-1.0` | `1.24.2` |
| gst_parse_oracle | in-tree | [`gst_parse_oracle.c`](gst_parse_oracle.c), [`gst_fields.h`](gst_fields.h) (generated by `tools/gen_gst_fields.py`) | LGPL-2.1-or-later (links LGPL GStreamer; oracle-only) | `cat "$OVT_ORACLES_STAMPS/gst_parse_oracle.version"` | `gst_parse_oracle (OVT tests/oracles) on gstreamer-codecparsers 1.24.2` |
| ffmpeg_mvs | in-tree | [`ffmpeg_mvs.c`](ffmpeg_mvs.c), linked against the FFmpeg above | LGPL-2.1-or-later (links FFmpeg; oracle-only) | `cat "$OVT_ORACLES_STAMPS/ffmpeg_mvs.version"` | `ffmpeg_mvs (OVT tests/oracles) on FFmpeg n7.1.1` |
| ldecod | 19.0 | <https://vcgit.hhi.fraunhofer.de/jvet/JM> tag JM-19.0 `ef2362d159339d583fc18cea2de4d4bed5a1417c`, `TRACE 1` | ITU-T/ISO reference software licence | `cat "$OVT_ORACLES_STAMPS/ldecod.version"` | `JM 19.0 TRACE=1` |
| lencod | 19.0 | as ldecod (stock) | ITU-T/ISO reference software licence | `cat "$OVT_ORACLES_STAMPS/lencod.version"` | `JM 19.0` |
| TAppEncoder | HM-18.0 | <https://vcgit.hhi.fraunhofer.de/jvet/HM> `fb4486d5ab5d0cd3b6a71659c7d5eb4509f2a4ce` | BSD-3-Clause | `cat "$OVT_ORACLES_STAMPS/TAppEncoder.version"` | `HM-18.0` |
| TAppDecoder | HM-18.0 | as TAppEncoder | BSD-3-Clause | `cat "$OVT_ORACLES_STAMPS/TAppDecoder.version"` | `HM-18.0` |
| TAppDecoder_trace | HM-18.0 | as TAppEncoder, `ENC_DEC_TRACE 1` | BSD-3-Clause | `cat "$OVT_ORACLES_STAMPS/TAppDecoder_trace.version"` | `HM-18.0` |
| hm_cu_dump | in-tree | [`hm_cu_dump.cpp`](hm_cu_dump.cpp), linked against the stock HM-18.0 libraries | BSD-3-Clause-Clear (links HM; oracle-only) | `cat "$OVT_ORACLES_STAMPS/hm_cu_dump.version"` | `hm_cu_dump (OVT tests/oracles) on HM-18.0` |
| dump_psnrhvs | 24f889aa | <https://gitlab.xiph.org/xiph/daala> `24f889aaabe33bc390b993d737d830b8dd367e61` (master; no releases) | BSD-2-Clause | `cat "$OVT_ORACLES_STAMPS/dump_psnrhvs.version"` | `Daala 24f889aaabe33bc390b993d737d830b8dd367e61` |
| dump_ciede2000 | 24f889aa | as dump_psnrhvs | BSD-2-Clause | `cat "$OVT_ORACLES_STAMPS/dump_ciede2000.version"` | `Daala 24f889aaabe33bc390b993d737d830b8dd367e61` |
| mpv | v0.39.0 | <https://github.com/mpv-player/mpv> `a0fba7be57f3822d967b04f0f6b6d6341e7516e7` | GPL-2.0-or-later (this build) | `mpv --no-config --version` | `mpv v0.39.0` |
| libplacebo | v7.349.0 | <https://code.videolan.org/videolan/libplacebo> `1fd3c7bde7b943fe8985c893310b5269a09b46c5` | LGPL-2.1-or-later | `pkg-config --modversion libplacebo` | `7.349.0` |
| uvq | c204ecc0 | <https://github.com/google/uvq> `c204ecc04f093e90c941cb126c7ac3d5beca0414` (PyTorch; UVQ 1.5 and 1.0) | Apache-2.0 | `cat "$OVT_ORACLES_STAMPS/uvq.version"` | `google/uvq c204ecc04f093e90c941cb126c7ac3d5beca0414` |
| torch | 2.8.0+cpu | [`requirements.txt`](requirements.txt) (CPU build, <https://download.pytorch.org/whl/cpu>) | BSD-3-Clause | `python-package:torch` | `2.8.0+cpu` |
| torchvision | 0.23.0+cpu | [`requirements.txt`](requirements.txt) (CPU build) | BSD-3-Clause | `python-package:torchvision` | `0.23.0+cpu` |
| bjontegaard | 1.3.0 | [`requirements.txt`](requirements.txt) | BSD-3-Clause | `python-package:bjontegaard` | `1.3.0` |
| siti-tools | 0.6.0 | [`requirements.txt`](requirements.txt) | MIT | `python-package:siti-tools` | `0.6.0` |
| scikit-image | 0.26.0 | [`requirements.txt`](requirements.txt) (for `dump_ciede2000`) | BSD-3-Clause | `python-package:scikit-image` | `0.26.0` |
| y4m | 1.1.1 | [`requirements.txt`](requirements.txt) (for `dump_ciede2000`) | Apache-2.0 | `python-package:y4m` | `1.1.1` |
<!-- oracle-versions:end -->

`gst_parse_oracle` (WI-0.562), `hm_cu_dump` and `ffmpeg_mvs` (WI-0.563) are built from the sources in this directory.
The other in-house oracle programs (`x264_ssim`, `mpv_capture.py`) are added to the image's `oracles` stage, and to this
table, by WI-0.565 and WI-1.575.

## Licences and distribution

The image holds GPL builds (FFmpeg with x264/x265, mpv, Bento4, MKVToolNix) and the patent-encumbered
H.264/HEVC encoders x264, x265, JM `lencod` and HM `TAppEncoder`. It is a test tool: it is never shipped
with OVT and nothing in it is linked into OVT. The GHCR package stays private (internal use); publishing it
needs the GPL source offer (the build scripts plus the pinned upstream sources) and the owner's patent
check first (project rules; chapter 15).
