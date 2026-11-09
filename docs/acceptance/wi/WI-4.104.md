# WI-4.104 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, clang 18.1, CMake 3.28.3, Ninja.
Date: 2026-11-09. Spec version: 1.0-rc14. Commit: the head of the WI-4.104 pull request.

## Test 1: pending on the conformance stream; checked here on HM-encoded streams

AT1: given the build, when the smoke program runs on `WPP_A_ericsson_MAIN_2`, then POCs 0..N print and the MD5 SEI
check reports OK for every picture. That is the CTest `ovt_plugin_hm_smoke_wpp_a` (labels `third_party;needs-corpus`),
built with `-DOVT_BUILD_PLUGIN_HM=ON`. It is skipped when the corpus file is missing and fails when
`OVT_REQUIRE_CORPORA=1`.

This container can reach neither `vcgit.hhi.fraunhofer.de` nor `www.itu.int`. Here, the HM source came from a GitHub
mirror fetched at the pinned commit `fb4486d5ab5d0cd3b6a71659c7d5eb4509f2a4ce`, which git verifies by object id.
The VERSIONS.md URL was pointed at that clone with `file://` for the local run only. The conformance stream was not
available, so the smoke program ran on streams encoded with HM-18.0's own TAppEncoder from the same commit:

```console
$ cmake --preset linux-x64-debug -DOVT_BUILD_PLUGIN_HM=ON
$ cmake --build --preset linux-x64-debug --target ovt_plugin_hm_smoke
[8/10] Completed 'tp_hm'
[10/10] Linking CXX executable plugins/ovt-plugin-hm/tests/ovt_plugin_hm_smoke
$ TAppEncoder -c encoder_randomaccess_main.cfg ... -f 8 --SEIDecodedPictureHash=1 --WaveFrontSynchro=1 -b wpp.bit
$ ovt_plugin_hm_smoke wpp.bit | grep ovt_plugin
ovt_plugin_hm_smoke: POC 0 MD5 OK
ovt_plugin_hm_smoke: POC 4 MD5 OK
ovt_plugin_hm_smoke: POC 2 MD5 OK
ovt_plugin_hm_smoke: POC 1 MD5 OK
ovt_plugin_hm_smoke: POC 3 MD5 OK
ovt_plugin_hm_smoke: POC 6 MD5 OK
ovt_plugin_hm_smoke: POC 5 MD5 OK
ovt_plugin_hm_smoke: POC 7 MD5 OK
ovt_plugin_hm_smoke: 8 pictures, POC 0..7, all hashes OK
$ echo $?
0
```

| Stream | Result |
|---|---|
| 416×240, 8 frames, random access, WPP, MD5 SEI | 8 pictures, all MD5 OK, exit 0 |
| 416×240, 4 frames, low delay, Main10 (10-bit), CRC SEI | 4 pictures, all CRC OK, exit 0 (16-bit `Pel` build) |
| the same without hash SEI | every picture `hash MISSING`, exit 1 |
| the WPP stream with one byte flipped | HM's own assertion in `TDecSlice.cpp`, exit 134 |

The CTest was also run with a stand-in corpus entry (`OVT_CORPORA_DIR` holding the WPP stream under
`jctvc_WPP_A_ericsson_MAIN_2/`): it passed with a JSON `.ok` record and with a bare `.ok` marker, failed with the
no-hash stream, was skipped without the entry, and failed without the entry when `OVT_REQUIRE_CORPORA=1`.

A pin that doesn't match makes the download step fail:

```console
$ cmake -DGIT=git -DURL=file://<mirror> -DTAG=HM-18.0 -DCOMMIT=0000000000000000000000000000000000000000 \
    -DDEST=<dir> -P third_party/hm/fetch.cmake
CMake Error: hm fetch.cmake: tag HM-18.0 of file://<mirror> is fb4486d5ab5d0cd3b6a71659c7d5eb4509f2a4ce, but
  third_party/VERSIONS.md pins 0000000000000000000000000000000000000000
```

The HM libraries also build with clang 18 and `-fsanitize=address,undefined` through `third_party/hm/CMakeLists.txt`.
Running the smoke program under UBSan stops in HM's own code (signed overflow, `SEIFilmGrainSynthesizer.cpp:688`,
see #345). No CI preset builds HM yet.

Still owed: the run on `WPP_A_ericsson_MAIN_2` itself, and a macOS arm64 build. Both need a machine that reaches
`vcgit.hhi.fraunhofer.de` and the ITU site, such as the Mac Mini:

```sh
cmake --preset macos-arm64-release -DOVT_BUILD_PLUGIN_HM=ON
cmake --build --preset macos-arm64-release --target ovt_plugin_hm_smoke
python3 tools/fetch_corpora.py --ids jctvc_WPP_A_ericsson_MAIN_2
OVT_REQUIRE_CORPORA=1 ctest --test-dir build/macos-arm64-release -R ovt_plugin_hm_smoke --output-on-failure
```

Record the output here.
